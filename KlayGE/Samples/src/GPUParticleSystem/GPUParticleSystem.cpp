#include <KlayGE/KlayGE.hpp>
#include <KlayGE/ThrowErr.hpp>
#include <KlayGE/Util.hpp>
#include <KlayGE/Math.hpp>
#include <KlayGE/Font.hpp>
#include <KlayGE/Renderable.hpp>
#include <KlayGE/RenderableHelper.hpp>
#include <KlayGE/RenderEngine.hpp>
#include <KlayGE/Sampler.hpp>
#include <KlayGE/RenderEffect.hpp>
#include <KlayGE/FrameBuffer.hpp>
#include <KlayGE/SceneManager.hpp>
#include <KlayGE/Context.hpp>
#include <KlayGE/ResLoader.hpp>
#include <KlayGE/RenderSettings.hpp>
#include <KlayGE/KMesh.hpp>
#include <KlayGE/RenderableHelper.hpp>
#include <KlayGE/SceneObjectHelper.hpp>
#include <KlayGE/ElementFormat.hpp>
#include <KlayGE/Timer.hpp>

#include <KlayGE/D3D9/D3D9RenderFactory.hpp>
#include <KlayGE/OpenGL/OGLRenderFactory.hpp>

#include <KlayGE/Input.hpp>
#include <KlayGE/DInput/DInputFactory.hpp>

#include <vector>
#include <sstream>
#include <fstream>
#include <ctime>
#include <boost/tuple/tuple.hpp>
#ifdef KLAYGE_COMPILER_MSVC
#pragma warning(push)
#pragma warning(disable: 4127 4512)
#endif
#include <boost/random.hpp>
#ifdef KLAYGE_COMPILER_MSVC
#pragma warning(pop)
#endif
#include <boost/bind.hpp>
#ifdef KLAYGE_COMPILER_MSVC
#pragma warning(push)
#pragma warning(disable: 4251 4275 4512 4702)
#endif
#include <boost/program_options.hpp>
#ifdef KLAYGE_COMPILER_MSVC
#pragma warning(pop)
#endif

#include "GPUParticleSystem.hpp"

using namespace std;
using namespace KlayGE;

namespace
{
	int const NUM_PARTICLE = 65536;

	float GetDensity(int x, int y, int z, std::vector<uint8_t> const & data, uint32_t vol_size)
	{
		if (x < 0)
		{
			x += vol_size;
		}
		if (y < 0)
		{
			y += vol_size;
		}
		if (z < 0)
		{
			z += vol_size;
		}

		x = x % vol_size;
		y = y % vol_size;
		z = z % vol_size;

		return data[((z * vol_size + y) * vol_size + x) * 4 + 3] / 255.0f - 0.5f;
	}

	TexturePtr CreateNoiseVolume(uint32_t vol_size)
	{
		boost::variate_generator<boost::lagged_fibonacci607, boost::uniform_real<float> > random_gen(boost::lagged_fibonacci607(), boost::uniform_real<float>(0, 255));

		std::vector<uint8_t> data(vol_size * vol_size * vol_size * 4);

		// Gen a bunch of random values
		for (size_t i = 0; i < data.size() / 4; ++ i)
		{
			data[i * 4 + 3] = static_cast<uint8_t>(random_gen());
		}

		// Generate normals from the density gradient
		float height_adjust = 0.5f;
		float3 normal;
		for (uint32_t z = 0; z < vol_size; ++ z)
		{
			for (uint32_t y = 0; y < vol_size; ++ y)
			{
				for (uint32_t x = 0; x < vol_size; ++ x)
				{
					normal.x() = (GetDensity(x + 1, y, z, data, vol_size) - GetDensity(x - 1, y, z, data, vol_size)) / height_adjust;
					normal.y() = (GetDensity(x, y + 1, z, data, vol_size) - GetDensity(x, y - 1, z, data, vol_size)) / height_adjust;
					normal.z() = (GetDensity(x, y, z + 1, data, vol_size) - GetDensity(x, y, z - 1, data, vol_size)) / height_adjust;

					normal = MathLib::normalize(normal);

					data[((z * vol_size + y) * vol_size + x) * 4 + 2] = static_cast<uint8_t>(MathLib::clamp(static_cast<int>((normal.x() / 2 + 0.5f) * 255.0f), 0, 255));
					data[((z * vol_size + y) * vol_size + x) * 4 + 1] = static_cast<uint8_t>(MathLib::clamp(static_cast<int>((normal.y() / 2 + 0.5f) * 255.0f), 0, 255));
					data[((z * vol_size + y) * vol_size + x) * 4 + 0] = static_cast<uint8_t>(MathLib::clamp(static_cast<int>((normal.z() / 2 + 0.5f) * 255.0f), 0, 255));
				}
			}
		}

		RenderFactory& rf = Context::Instance().RenderFactoryInstance();
		TexturePtr vol_tex = rf.MakeTexture3D(vol_size, vol_size, vol_size, 1, EF_ARGB8);
		{
			Texture::Mapper mapper(*vol_tex, 0, TMA_Write_Only, 0, 0, 0, vol_size, vol_size, vol_size);
			uint8_t* p = mapper.Pointer<uint8_t>();
			for (uint32_t z = 0; z < vol_size; ++ z)
			{
				for (uint32_t y = 0; y < vol_size; ++ y)
				{
					memcpy(p, &data[(z * vol_size + y) * vol_size], vol_size * 4);
					p += mapper.RowPitch();
				}
				p += mapper.SlicePitch() - mapper.RowPitch() * vol_size;
			}
		}

		return vol_tex;
	}

	struct Particle
	{
		float3 pos;
		float3 vel;
		float birth_time;
	};

	class RenderParticles : public RenderableHelper
	{
	public:
		explicit RenderParticles(int max_num_particles)
			: RenderableHelper(L"RenderParticles"),
				tex_width_(256), tex_height_(max_num_particles / 256)
		{
			RenderFactory& rf = Context::Instance().RenderFactoryInstance();

			GraphicsBufferPtr tex0 = rf.MakeVertexBuffer(BU_Static);
			GraphicsBufferPtr pos = rf.MakeVertexBuffer(BU_Static);
			GraphicsBufferPtr ib = rf.MakeIndexBuffer(BU_Static);

			rl_ = rf.MakeRenderLayout();
			rl_->TopologyType(RenderLayout::TT_TriangleStrip);
			rl_->BindVertexStream(tex0, boost::make_tuple(vertex_element(VEU_TextureCoord, 0, EF_GR32F)),
									RenderLayout::ST_Geometry, max_num_particles);
			rl_->BindVertexStream(pos, boost::make_tuple(vertex_element(VEU_Position, 0, EF_GR32F)),
									RenderLayout::ST_Instance, 1);
			rl_->BindIndexStream(ib, EF_R16);

			float2 texs[] =
			{
				float2(0.0f, 0.0f),
				float2(1.0f, 0.0f),
				float2(0.0f, 1.0f),
				float2(1.0f, 1.0f),
			};

			uint16_t indices[] =
			{
				0, 1, 2, 3,
			};

			tex0->Resize(sizeof(texs));
			{
				GraphicsBuffer::Mapper mapper(*tex0, BA_Write_Only);
				std::copy(&texs[0], &texs[4], mapper.Pointer<float2>());
			}

			pos->Resize(max_num_particles * sizeof(float2));
			for (int i = 0; i < max_num_particles; ++ i)
			{
				GraphicsBuffer::Mapper mapper(*pos, BA_Write_Only);
				mapper.Pointer<float2>()[i] = float2((i % tex_width_ + 0.5f) / tex_width_,
					(static_cast<float>(i) / tex_width_) / tex_height_);
			}

			ib->Resize(sizeof(indices));
			{
				GraphicsBuffer::Mapper mapper(*ib, BA_Write_Only);
				std::copy(&indices[0], &indices[4], mapper.Pointer<uint16_t>());
			}

			technique_ = rf.LoadEffect("GPUParticleSystem.kfx")->TechniqueByName("Particles");

			particle_tex_ = LoadTexture("particle.dds");
			*(technique_->Effect().ParameterByName("particle_sampler")) = particle_tex_;

			noise_vol_tex_ = CreateNoiseVolume(32);
			*(technique_->Effect().ParameterByName("noise_vol_sampler")) = noise_vol_tex_;
		}

		void SceneTexture(TexturePtr tex)
		{
			*(technique_->Effect().ParameterByName("scene_sampler")) = tex;
		}

		void OnRenderBegin()
		{
			*(technique_->Effect().ParameterByName("particle_pos_sampler")) = particle_pos_tex_;

			App3DFramework const & app = Context::Instance().AppInstance();

			float4x4 const & view = app.ActiveCamera().ViewMatrix();
			float4x4 const & proj = app.ActiveCamera().ProjMatrix();
			float4x4 const inv_proj = MathLib::inverse(proj);

			*(technique_->Effect().ParameterByName("View")) = view;
			*(technique_->Effect().ParameterByName("Proj")) = proj;
			*(technique_->Effect().ParameterByName("inv_view")) = MathLib::inverse(view);

			*(technique_->Effect().ParameterByName("point_radius")) = 0.1f;
			*(technique_->Effect().ParameterByName("init_pos_life")) = float4(0, 0, 0, 8);

			RenderEngine& re = Context::Instance().RenderFactoryInstance().RenderEngineInstance();
			float4 const & texel_to_pixel = re.TexelToPixelOffset() * 2;
			float const x_offset = texel_to_pixel.x() / re.CurFrameBuffer()->Width();
			float const y_offset = texel_to_pixel.y() / re.CurFrameBuffer()->Height();
			*(technique_->Effect().ParameterByName("offset")) = float2(x_offset, y_offset);

			*(technique_->Effect().ParameterByName("upper_left")) = MathLib::transform_coord(float3(-1, 1, 1), inv_proj);
			*(technique_->Effect().ParameterByName("upper_right")) = MathLib::transform_coord(float3(1, 1, 1), inv_proj);
			*(technique_->Effect().ParameterByName("lower_left")) = MathLib::transform_coord(float3(-1, -1, 1), inv_proj);
			*(technique_->Effect().ParameterByName("lower_right")) = MathLib::transform_coord(float3(1, -1, 1), inv_proj);
		}

		void PosTexture(TexturePtr particle_pos_tex)
		{
			particle_pos_tex_ = particle_pos_tex;
		}

	private:
		int tex_width_, tex_height_;

		TexturePtr particle_tex_;
		TexturePtr particle_pos_tex_;
		TexturePtr noise_vol_tex_;
	};

	class ParticlesObject : public SceneObjectHelper
	{
	public:
		explicit ParticlesObject(int max_num_particles)
			: SceneObjectHelper(RenderablePtr(new RenderParticles(max_num_particles)), SOA_Cullable)
		{
		}

		void PosTexture(TexturePtr particle_pos_tex)
		{
			checked_pointer_cast<RenderParticles>(renderable_)->PosTexture(particle_pos_tex);
		}
	};

	class GPUParticleSystem : public RenderablePlane
	{
	public:
		GPUParticleSystem(int max_num_particles, TexturePtr terrain_height_map, TexturePtr terrain_normal_map)
			: RenderablePlane(2, 2, 1, 1, true),
				max_num_particles_(max_num_particles),
				tex_width_(256), tex_height_((max_num_particles + 255) / 256),
				model_mat_(float4x4::Identity()),
				rt_index_(true), accumulate_time_(0),
				random_gen_(boost::lagged_fibonacci607(), boost::uniform_real<float>(-0.05f, 0.05f))
		{
			RenderFactory& rf = Context::Instance().RenderFactoryInstance();
			RenderEngine& re = rf.RenderEngineInstance();

			technique_ = rf.LoadEffect("GPUParticleSystem.kfx")->TechniqueByName("Update");

			particle_pos_texture_[0] = rf.MakeTexture2D(tex_width_, tex_height_, 1, EF_ABGR32F);
			particle_pos_texture_[1] = rf.MakeTexture2D(tex_width_, tex_height_, 1, EF_ABGR32F);
			particle_vel_texture_[0] = rf.MakeTexture2D(tex_width_, tex_height_, 1, EF_ABGR32F);
			particle_vel_texture_[1] = rf.MakeTexture2D(tex_width_, tex_height_, 1, EF_ABGR32F);
			for (int i = 0; i < 2; ++ i)
			{
				Texture::Mapper mapper(*particle_pos_texture_[i], 0, TMA_Write_Only, 0, 0, tex_width_, tex_height_);
				float4* pos_init = mapper.Pointer<float4>();
				for (int y = 0; y < tex_height_; ++ y)
				{
					for (int x = 0; x < tex_width_; ++ x)
					{
						pos_init[x] = float4(0, 0, 0, -1);
					}
					pos_init += mapper.RowPitch() / sizeof(pos_init[0]);
				}
			}

			pos_vel_rt_buffer_[0] = rf.MakeFrameBuffer();
			pos_vel_rt_buffer_[0]->Attach(FrameBuffer::ATT_Color0, rf.Make2DRenderView(*particle_pos_texture_[0], 0));
			pos_vel_rt_buffer_[0]->Attach(FrameBuffer::ATT_Color1, rf.Make2DRenderView(*particle_vel_texture_[0], 0));

			pos_vel_rt_buffer_[1] = rf.MakeFrameBuffer();
			pos_vel_rt_buffer_[1]->Attach(FrameBuffer::ATT_Color0, rf.Make2DRenderView(*particle_pos_texture_[1], 0));
			pos_vel_rt_buffer_[1]->Attach(FrameBuffer::ATT_Color1, rf.Make2DRenderView(*particle_vel_texture_[1], 0));

			FrameBufferPtr screen_buffer = re.CurFrameBuffer();
			pos_vel_rt_buffer_[0]->GetViewport().camera = pos_vel_rt_buffer_[1]->GetViewport().camera
				= screen_buffer->GetViewport().camera;

			particle_birth_time_ = rf.MakeTexture2D(tex_width_, tex_height_, 1, EF_R32F);
			*(technique_->Effect().ParameterByName("particle_birth_time_sampler")) = particle_birth_time_;

			TexturePtr particle_init_vel;
			particle_init_vel = rf.MakeTexture2D(tex_width_, tex_height_, 1, EF_ABGR32F);
			{
				Texture::Mapper mapper(*particle_init_vel, 0, TMA_Write_Only, 0, 0, tex_width_, tex_height_);
				float4* vel_init = mapper.Pointer<float4>();
				for (int y = 0; y < tex_height_; ++ y)
				{
					for (int x = 0; x < tex_width_; ++ x)
					{
						float const angel = random_gen_() / 0.05f * PI;
						float const r = random_gen_() * 3;

						vel_init[x] = float4(r * cos(angel), 0.2f + abs(random_gen_()) * 3, r * sin(angel), 0);
					}
					vel_init += mapper.RowPitch() / sizeof(vel_init[0]);
				}
			}
			*(technique_->Effect().ParameterByName("particle_init_vel_sampler")) = particle_init_vel;

			particle_pos_sampler_param_ = technique_->Effect().ParameterByName("particle_pos_sampler");
			particle_vel_sampler_param_ = technique_->Effect().ParameterByName("particle_vel_sampler");
			accumulate_time_param_ = technique_->Effect().ParameterByName("accumulate_time");
			elapse_time_param_ = technique_->Effect().ParameterByName("elapse_time");

			*(technique_->Effect().ParameterByName("init_pos_life")) = float4(0, 0, 0, 8);
			*(technique_->Effect().ParameterByName("height_map_sampler")) = terrain_height_map;
			*(technique_->Effect().ParameterByName("normal_map_sampler")) = terrain_normal_map;
		}

		void ModelMatrix(float4x4 const & model)
		{
			model_mat_ = model;
			*(technique_->Effect().ParameterByName("ps_model_mat")) = model;
		}

		float4x4 const & ModelMatrix() const
		{
			return model_mat_;
		}

		void AutoEmit(float freq)
		{
			inv_emit_freq_ = 1.0f / freq;

			float time = 0;

			Texture::Mapper mapper(*particle_birth_time_, 0, TMA_Write_Only, 0, 0, tex_width_, tex_height_);
			float* birth_time_init = mapper.Pointer<float>();
			for (int y = 0; y < tex_height_; ++ y)
			{
				for (int x = 0; x < tex_width_; ++ x)
				{
					birth_time_init[x] = time;
					time += inv_emit_freq_;
				}
				birth_time_init += mapper.RowPitch() / sizeof(birth_time_init[0]);
			}
		}

		void Update(float elapse_time)
		{
			RenderEngine& re = Context::Instance().RenderFactoryInstance().RenderEngineInstance();
			re.BindFrameBuffer(pos_vel_rt_buffer_[rt_index_]);

			accumulate_time_ += elapse_time;
			if (accumulate_time_ >= max_num_particles_ * inv_emit_freq_)
			{
				accumulate_time_ = 0;
			}

			*elapse_time_param_ = elapse_time;
			*accumulate_time_param_ = accumulate_time_;

			*particle_pos_sampler_param_ = this->PosTexture();
			*particle_vel_sampler_param_ = this->VelTexture();

			this->Render();

			rt_index_ = !rt_index_;
		}

		TexturePtr PosTexture() const
		{
			return particle_pos_texture_[!rt_index_];
		}

		TexturePtr VelTexture() const
		{
			return particle_vel_texture_[!rt_index_];
		}

	private:
		int max_num_particles_;
		int tex_width_, tex_height_;

		float4x4 model_mat_;

		TexturePtr particle_pos_texture_[2];
		TexturePtr particle_vel_texture_[2];

		FrameBufferPtr pos_vel_rt_buffer_[2];

		bool rt_index_;

		TexturePtr particle_birth_time_;

		float accumulate_time_;
		float inv_emit_freq_;

		boost::variate_generator<boost::lagged_fibonacci607, boost::uniform_real<float> > random_gen_;

		RenderEffectParameterPtr vel_offset_param_;
		RenderEffectParameterPtr particle_pos_sampler_param_;
		RenderEffectParameterPtr particle_vel_sampler_param_;
		RenderEffectParameterPtr accumulate_time_param_;
		RenderEffectParameterPtr elapse_time_param_;
	};

	class TerrainRenderable : public RenderablePlane
	{
	public:
		explicit TerrainRenderable(TexturePtr height_map, TexturePtr normal_map)
			: RenderablePlane(4, 4, 64, 64, true)
		{
			RenderFactory& rf = Context::Instance().RenderFactoryInstance();

			technique_ = rf.LoadEffect("GPUParticleSystem.kfx")->TechniqueByName("Terrain");

			TexturePtr height_32f = rf.MakeTexture2D(height_map->Width(0), height_map->Height(0), 1, EF_R32F);
			height_map->CopyToTexture(*height_32f);

			*(technique_->Effect().ParameterByName("height_map_sampler")) = height_32f;
			*(technique_->Effect().ParameterByName("normal_map_sampler")) = normal_map;

			*(technique_->Effect().ParameterByName("grass_sampler")) = LoadTexture("grass.dds");
		}

		void OnRenderBegin()
		{
			App3DFramework const & app = Context::Instance().AppInstance();

			float4x4 view = app.ActiveCamera().ViewMatrix();
			float4x4 proj = app.ActiveCamera().ProjMatrix();

			*(technique_->Effect().ParameterByName("View")) = view;
			*(technique_->Effect().ParameterByName("Proj")) = proj;

			Camera const & camera = app.ActiveCamera();
			*(technique_->Effect().ParameterByName("depth_min")) = camera.NearPlane();
			*(technique_->Effect().ParameterByName("inv_depth_range")) = 1 / (camera.FarPlane() - camera.NearPlane());
		}

	private:
		float4x4 model_;
	};

	class TerrainObject : public SceneObjectHelper
	{
	public:
		TerrainObject(TexturePtr height_map, TexturePtr normal_map)
			: SceneObjectHelper(RenderablePtr(new TerrainRenderable(height_map, normal_map)), SOA_Cullable)
		{
		}
	};

	class BlendPostProcess : public PostProcess
	{
	public:
		BlendPostProcess()
			: PostProcess(Context::Instance().RenderFactoryInstance().LoadEffect("GPUParticleSystem.kfx")->TechniqueByName("Blend"))
		{
		}

		void TexWithAlpha(TexturePtr const & tex)
		{
			*(technique_->Effect().ParameterByName("tex_with_alpha_sampler")) = tex;
		}
	};


	boost::shared_ptr<GPUParticleSystem> gpu_ps;


	enum
	{
		Exit
	};

	InputActionDefine actions[] =
	{
		InputActionDefine(Exit, KS_Escape)
	};

	bool ConfirmDevice()
	{
		RenderFactory& rf = Context::Instance().RenderFactoryInstance();
		RenderEngine& re = rf.RenderEngineInstance();
		RenderDeviceCaps const & caps = re.DeviceCaps();
		if (caps.max_shader_model < 3)
		{
			return false;
		}
		if (caps.max_simultaneous_rts < 2)
		{
			return false;
		}

		try
		{
			TexturePtr temp_tex = rf.MakeTexture2D(256, 256, 1, EF_ABGR32F);
			rf.Make2DRenderView(*temp_tex, 0);

			temp_tex = rf.MakeTexture2D(256, 256, 1, EF_R32F);
			rf.Make2DRenderView(*temp_tex, 0);
		}
		catch (...)
		{
			return false;
		}

		return true;
	}
}


int main()
{
	ResLoader::Instance().AddPath("../../media/Common");
	ResLoader::Instance().AddPath("../../media/GPUParticleSystem");

	RenderSettings settings;

	{
		int octree_depth = 3;
		int width = 800;
		int height = 600;
		int color_fmt = 13; // EF_ARGB8
		bool full_screen = false;

		boost::program_options::options_description desc("Configuration");
		desc.add_options()
			("context.render_factory", boost::program_options::value<std::string>(), "Render Factory")
			("context.input_factory", boost::program_options::value<std::string>(), "Input Factory")
			("context.scene_manager", boost::program_options::value<std::string>(), "Scene Manager")
			("octree.depth", boost::program_options::value<int>(&octree_depth)->default_value(3), "Octree depth")
			("screen.width", boost::program_options::value<int>(&width)->default_value(800), "Screen Width")
			("screen.height", boost::program_options::value<int>(&height)->default_value(600), "Screen Height")
			("screen.color_fmt", boost::program_options::value<int>(&color_fmt)->default_value(13), "Screen Color Format")
			("screen.fullscreen", boost::program_options::value<bool>(&full_screen)->default_value(false), "Full Screen");

		std::ifstream cfg_fs(ResLoader::Instance().Locate("KlayGE.cfg").c_str());
		if (cfg_fs)
		{
			boost::program_options::variables_map vm;
			boost::program_options::store(boost::program_options::parse_config_file(cfg_fs, desc), vm);
			boost::program_options::notify(vm);

			if (vm.count("context.render_factory"))
			{
				std::string rf_name = vm["context.render_factory"].as<std::string>();
				if ("D3D9" == rf_name)
				{
					Context::Instance().RenderFactoryInstance(D3D9RenderFactoryInstance());
				}
				if ("OpenGL" == rf_name)
				{
					Context::Instance().RenderFactoryInstance(OGLRenderFactoryInstance());
				}
			}
			else
			{
				Context::Instance().RenderFactoryInstance(D3D9RenderFactoryInstance());
			}

			if (vm.count("context.input_factory"))
			{
				std::string if_name = vm["context.input_factory"].as<std::string>();
				if ("DInput" == if_name)
				{
					Context::Instance().InputFactoryInstance(DInputFactoryInstance());
				}
			}
			else
			{
				Context::Instance().InputFactoryInstance(DInputFactoryInstance());
			}
		}
		else
		{
			Context::Instance().RenderFactoryInstance(D3D9RenderFactoryInstance());
			Context::Instance().InputFactoryInstance(DInputFactoryInstance());
		}

		settings.width = width;
		settings.height = height;
		settings.color_fmt = static_cast<ElementFormat>(color_fmt);
		settings.full_screen = full_screen;
		settings.ConfirmDevice = ConfirmDevice;
	}

	GPUParticleSystemApp app("GPU Particle System", settings);
	app.Create();
	app.Run();

	return 0;
}

GPUParticleSystemApp::GPUParticleSystemApp(std::string const & name, RenderSettings const & settings)
							: App3DFramework(name, settings)
{
}

void GPUParticleSystemApp::InitObjects()
{
	font_ = Context::Instance().RenderFactoryInstance().MakeFont("gkai00mp.kfont", 16);

	this->LookAt(float3(-1.2f, 2.2f, -1.2f), float3(0, 0.5f, 0));
	this->Proj(0.01f, 100);

	fpcController_.AttachCamera(this->ActiveCamera());
	fpcController_.Scalers(0.05f, 0.1f);

	InputEngine& inputEngine(Context::Instance().InputFactoryInstance().InputEngineInstance());
	InputActionMap actionMap;
	actionMap.AddActions(actions, actions + sizeof(actions) / sizeof(actions[0]));

	action_handler_t input_handler(new input_signal);
	input_handler->connect(boost::bind(&GPUParticleSystemApp::InputHandler, this, _1, _2));
	inputEngine.ActionMap(actionMap, input_handler, true);

	particles_.reset(new ParticlesObject(NUM_PARTICLE));
	particles_->AddToSceneManager();

	TexturePtr terrain_height = LoadTexture("terrain_height.dds");
	TexturePtr terrain_normal = LoadTexture("terrain_normal.dds");

	gpu_ps.reset(new GPUParticleSystem(NUM_PARTICLE, terrain_height, terrain_normal));
	gpu_ps->AutoEmit(500);

	terrain_.reset(new TerrainObject(terrain_height, terrain_normal));
	terrain_->AddToSceneManager();

	RenderFactory& rf = Context::Instance().RenderFactoryInstance();
	RenderEngine& re = rf.RenderEngineInstance();

	FrameBufferPtr screen_buffer = re.CurFrameBuffer();
	
	scene_buffer_ = rf.MakeFrameBuffer();
	scene_buffer_->GetViewport().camera = screen_buffer->GetViewport().camera;
	fog_buffer_ = rf.MakeFrameBuffer();
	fog_buffer_->GetViewport().camera = screen_buffer->GetViewport().camera;

	blend_pp_.reset(new BlendPostProcess);
}

void GPUParticleSystemApp::OnResize(uint32_t width, uint32_t height)
{
	App3DFramework::OnResize(width, height);

	RenderFactory& rf = Context::Instance().RenderFactoryInstance();
	RenderEngine& re = rf.RenderEngineInstance();

	scene_tex_ = rf.MakeTexture2D(width, height, 1, EF_ABGR16F);
	scene_buffer_->Attach(FrameBuffer::ATT_Color0, rf.Make2DRenderView(*scene_tex_, 0));
	scene_buffer_->Attach(FrameBuffer::ATT_DepthStencil, re.CurFrameBuffer()->Attached(FrameBuffer::ATT_DepthStencil));

	fog_tex_ = rf.MakeTexture2D(width, height, 1, EF_ABGR16F);
	fog_buffer_->Attach(FrameBuffer::ATT_Color0, rf.Make2DRenderView(*fog_tex_, 0));
	fog_buffer_->Attach(FrameBuffer::ATT_DepthStencil, re.CurFrameBuffer()->Attached(FrameBuffer::ATT_DepthStencil));

	checked_pointer_cast<RenderParticles>(particles_->GetRenderable())->SceneTexture(scene_tex_);

	blend_pp_->Source(scene_tex_, scene_buffer_->RequiresFlipping());
	blend_pp_->Destinate(FrameBufferPtr());
}

void GPUParticleSystemApp::InputHandler(InputEngine const & /*sender*/, InputAction const & action)
{
	switch (action.first)
	{
	case Exit:
		this->Quit();
		break;
	}
}

uint32_t GPUParticleSystemApp::DoUpdate(uint32_t pass)
{
	RenderFactory& rf = Context::Instance().RenderFactoryInstance();
	RenderEngine& re = rf.RenderEngineInstance();

	switch (pass)
	{
	case 0:
		re.BindFrameBuffer(scene_buffer_);
		re.CurFrameBuffer()->Clear(FrameBuffer::CBM_Color | FrameBuffer::CBM_Depth, Color(0.2f, 0.4f, 0.6f, 1), 1.0f, 0);

		terrain_->Visible(true);
		particles_->Visible(false);
		return App3DFramework::URV_Need_Flush;

	case 1:
		{
			terrain_->Visible(false);
			particles_->Visible(true);

			float4x4 mat = MathLib::translation(0.0f, 0.7f, 0.0f);
			gpu_ps->ModelMatrix(mat);

			gpu_ps->Update(static_cast<float>(timer_.elapsed()));
			timer_.restart();

			checked_pointer_cast<ParticlesObject>(particles_)->PosTexture(gpu_ps->PosTexture());

			re.BindFrameBuffer(fog_buffer_);
			re.CurFrameBuffer()->Clear(FrameBuffer::CBM_Color | FrameBuffer::CBM_Depth, Color(0, 0, 0, 0), 1.0f, 0);

			return App3DFramework::URV_Need_Flush;
		}

	default:
		{
			terrain_->Visible(false);
			particles_->Visible(false);

			checked_pointer_cast<BlendPostProcess>(blend_pp_)->TexWithAlpha(fog_tex_);

			re.BindFrameBuffer(FrameBufferPtr());
			re.CurFrameBuffer()->Clear(FrameBuffer::CBM_Color | FrameBuffer::CBM_Depth, Color(0.2f, 0.4f, 0.6f, 1), 1.0f, 0);

			blend_pp_->Apply();

			std::wostringstream stream;
			stream << this->FPS();

			font_->RenderText(0, 0, Color(1, 1, 0, 1), L"GPU Particle System");
			font_->RenderText(0, 18, Color(1, 1, 0, 1), stream.str().c_str());

			return App3DFramework::URV_Need_Flush | App3DFramework::URV_Finished;
		}
	}
}
