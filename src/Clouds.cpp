#include <string.h>
#include "AudibleInstruments.hpp"
#include "dsp/samplerate.hpp"
#include "dsp/ringbuffer.hpp"
#include "dsp/digital.hpp"
#include "dsp/vumeter.hpp"
#include "clouds/dsp/granular_processor.h"


struct Clouds : Module {
  enum ParamIds {
    POSITION_PARAM,
    SIZE_PARAM,
    PITCH_PARAM,
    IN_GAIN_PARAM,
    DENSITY_PARAM,
    TEXTURE_PARAM,
    BLEND_PARAM,
    SPREAD_PARAM,
    FEEDBACK_PARAM,
    REVERB_PARAM,
    FREEZE_PARAM,
#ifdef PARASITES
    REVERSE_PARAM,
#endif     
    NUM_PARAMS
  };
  enum InputIds {
    FREEZE_INPUT,
    TRIG_INPUT,
    POSITION_INPUT,
    SIZE_INPUT,
    PITCH_INPUT,
    BLEND_INPUT,
    SPREAD_INPUT,
    FEEDBACK_INPUT,
    REVERB_INPUT,
    IN_L_INPUT,
    IN_R_INPUT,
    DENSITY_INPUT,
    TEXTURE_INPUT,
    NUM_INPUTS
  };
  enum OutputIds {
    OUT_L_OUTPUT,
    OUT_R_OUTPUT,
    NUM_OUTPUTS
  };
	enum LightIds {
		FREEZE_LIGHT,
#ifdef PARASITES
    REVERSE_LIGHT,
#endif   
		NUM_LIGHTS
	};

  SampleRateConverter<2> inputSrc;
  SampleRateConverter<2> outputSrc;
  DoubleRingBuffer<Frame<2>, 256> inputBuffer;
  DoubleRingBuffer<Frame<2>, 256> outputBuffer;

  clouds::PlaybackMode playbackmode =  clouds::PLAYBACK_MODE_GRANULAR;
  
  
  int buffersize = 1;
  int currentbuffersize = 1;
  bool lofi = false;
  bool mono = false;
  uint8_t *block_mem;
  uint8_t *block_ccm;
  clouds::GranularProcessor *processor;

  bool triggered = false;
  float freezeLight = 0.0;
  bool freeze = false;
#ifdef PARASITES
  bool reverse = false;
  float reverseLight = 0.0;
  SchmittTrigger reverseTrigger;
#endif
  SchmittTrigger freezeTrigger;

  Clouds();
  ~Clouds();
  void step();
  
  
	json_t *toJson() override {
		json_t *rootJ = json_object();
    //playbackmode, lofi, mono
		json_object_set_new(rootJ, "playbackmode", json_integer(playbackmode));
    json_object_set_new(rootJ, "lofi", json_integer(lofi));
    json_object_set_new(rootJ, "mono", json_integer(mono));
    json_object_set_new(rootJ, "freeze", json_integer(freeze));
    json_object_set_new(rootJ, "buffersize", json_integer(buffersize));
#ifdef PARASITES
    json_object_set_new(rootJ, "reverse", json_integer(reverse));
#endif
		return rootJ;
	}

	void fromJson(json_t *rootJ) override {
		json_t *playbackmodeJ = json_object_get(rootJ, "playbackmode");
		if (playbackmodeJ) {
			playbackmode = (clouds::PlaybackMode)json_integer_value(playbackmodeJ);
		}
    json_t *lofiJ = json_object_get(rootJ, "lofi");
		if (lofiJ) {
			lofi = json_integer_value(lofiJ);
		}
    json_t *monoJ = json_object_get(rootJ, "mono");
		if (monoJ) {
			mono = json_integer_value(monoJ);
		}
    json_t *freezeJ = json_object_get(rootJ, "freeze");
		if (freezeJ) {
			freeze = json_integer_value(freezeJ);
		}
    json_t *buffersizeJ = json_object_get(rootJ, "buffersize");
		if (buffersizeJ) {
			buffersize = json_integer_value(buffersizeJ);
		}      
#ifdef PARASITES
    json_t *reverseJ = json_object_get(rootJ, "reverse");
		if (reverseJ) {
			reverse = json_integer_value(reverseJ);
		}    
#endif    
	}
  
};


Clouds::Clouds() : Module(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS) {
  const int memLen = 118784;
  const int ccmLen = 65536 - 128;
  block_mem = new uint8_t[memLen]();
  block_ccm = new uint8_t[ccmLen]();
  processor = new clouds::GranularProcessor();
  memset(processor, 0, sizeof(*processor));

  freezeTrigger.setThresholds(0.0, 1.0);
#ifdef PARASITES
  reverseTrigger.setThresholds(0.0, 1.0);   
#endif  
  processor->Init(block_mem, memLen, block_ccm, ccmLen);
}

Clouds::~Clouds() {
  delete processor;
  delete[] block_mem;
  delete[] block_ccm;
}

void Clouds::step() {
  // Get input
  if (!inputBuffer.full()) {
    Frame<2> inputFrame;
    inputFrame.samples[0] = inputs[IN_L_INPUT].value * params[IN_GAIN_PARAM].value / 5.0;
    inputFrame.samples[1] = inputs[IN_R_INPUT].active ? inputs[IN_R_INPUT].value * params[IN_GAIN_PARAM].value / 5.0 : inputFrame.samples[0];
    inputBuffer.push(inputFrame);
  }

  // Trigger
  if (inputs[TRIG_INPUT].value >= 1.0) {
    triggered = true;
  }

  // Render frames
  if (outputBuffer.empty()) {
    clouds::ShortFrame input[32] = {};
    // Convert input buffer
    {
      inputSrc.setRatio(32000.0 / engineGetSampleRate());
      Frame<2> inputFrames[32];
      int inLen = inputBuffer.size();
      int outLen = 32;
      inputSrc.process(inputBuffer.startData(), &inLen, inputFrames, &outLen);
      inputBuffer.startIncr(inLen);

      // We might not fill all of the input buffer if there is a deficiency, but this cannot be avoided due to imprecisions between the input and output SRC.
      for (int i = 0; i < outLen; i++) {
        input[i].l = clampf(inputFrames[i].samples[0] * 32767.0, -32768, 32767);
        input[i].r = clampf(inputFrames[i].samples[1] * 32767.0, -32768, 32767);
      }
    }
    if(currentbuffersize != buffersize){
      //re-init processor with new size
      delete processor;
      delete[] block_mem;
      int memLen = 118784*buffersize;
      const int ccmLen = 65536 - 128;
      block_mem = new uint8_t[memLen]();
      processor = new clouds::GranularProcessor();
      memset(processor, 0, sizeof(*processor));
      processor->Init(block_mem, memLen, block_ccm, ccmLen);
      currentbuffersize = buffersize;
    }

    // Set up processor
    processor->set_num_channels(mono ? 1 : 2);
    processor->set_low_fidelity(lofi);
    // TODO Support the other modes
    processor->set_playback_mode(playbackmode);
    processor->Prepare();

    
    if (freezeTrigger.process(params[FREEZE_PARAM].value)) {
       freeze = !freeze;
    } 
    

    
    clouds::Parameters* p = processor->mutable_parameters();
    p->trigger = triggered;
    p->gate = triggered;
    p->freeze = (inputs[FREEZE_INPUT].value >= 1.0 || freeze);
    p->position = clampf(params[POSITION_PARAM].value + inputs[POSITION_INPUT].value / 5.0, 0.0, 1.0);
    p->size = clampf(params[SIZE_PARAM].value + inputs[SIZE_INPUT].value / 5.0, 0.0, 1.0);
    p->pitch = clampf((params[PITCH_PARAM].value + inputs[PITCH_INPUT].value) * 12.0, -48.0, 48.0);
    p->density = clampf(params[DENSITY_PARAM].value + inputs[DENSITY_INPUT].value / 5.0, 0.0, 1.0);
    p->texture = clampf(params[TEXTURE_PARAM].value + inputs[TEXTURE_INPUT].value / 5.0, 0.0, 1.0);
    float blend = clampf(params[BLEND_PARAM].value + inputs[BLEND_INPUT].value / 5.0, 0.0, 1.0);
    p->dry_wet = blend;
    p->stereo_spread =  clampf(params[SPREAD_PARAM].value + inputs[SPREAD_INPUT].value / 5.0, 0.0, 1.0);;
    p->feedback =  clampf(params[FEEDBACK_PARAM].value + inputs[FEEDBACK_INPUT].value / 5.0, 0.0, 1.0);;
    p->reverb =  clampf(params[REVERB_PARAM].value + inputs[REVERB_INPUT].value / 5.0, 0.0, 1.0);;

#ifdef PARASITES
    if (reverseTrigger.process(params[REVERSE_PARAM].value)) {
       reverse = !reverse;
    } 
    p->granular.reverse = reverse;
    lights[REVERSE_LIGHT].setBrightness(p->granular.reverse ? 1.0 : 0.0);
#endif

    clouds::ShortFrame output[32];
    processor->Process(input, output, 32);
    
    lights[FREEZE_LIGHT].setBrightness(p->freeze ? 1.0 : 0.0);

    // Convert output buffer
    {
      Frame<2> outputFrames[32];
      for (int i = 0; i < 32; i++) {
        outputFrames[i].samples[0] = output[i].l / 32768.0;
        outputFrames[i].samples[1] = output[i].r / 32768.0;
      }

      outputSrc.setRatio( engineGetSampleRate() / 32000.0);
      int inLen = 32;
      int outLen = outputBuffer.capacity();
      outputSrc.process(outputFrames, &inLen, outputBuffer.endData(), &outLen);
      outputBuffer.endIncr(outLen);
    }

    triggered = false;
  }

  // Set output
  if (!outputBuffer.empty()) {
    Frame<2> outputFrame = outputBuffer.shift();
    outputs[OUT_L_OUTPUT].value = 5.0 * outputFrame.samples[0];
    outputs[OUT_R_OUTPUT].value = 5.0 * outputFrame.samples[1];
  }
}




CloudsWidget::CloudsWidget() {
  Clouds *module = new Clouds();
  setModule(module);
  box.size = Vec(15*18+67*3-6, 380);

  {
    Panel *panel = new LightPanel();
#ifdef PARASITES  
    panel->backgroundImage = Image::load(assetPlugin(plugin, "res/Neil.png"));
#else
    panel->backgroundImage = Image::load(assetPlugin(plugin, "res/Joni.png"));
#endif

    panel->box.size = box.size;
    addChild(panel);
  }


  // TODO
  // addParam(createParam<MediumMomentarySwitch>(Vec(211, 51), module, Clouds::POSITION_PARAM, 0.0, 1.0, 0.5));
  // addParam(createParam<MediumMomentarySwitch>(Vec(239, 51), module, Clouds::POSITION_PARAM, 0.0, 1.0, 0.5));

  addParam(createParam<Rogan3PSRed>(Vec(28, 94), module, Clouds::POSITION_PARAM, 0.0, 1.0, 0.5));
  addParam(createParam<Rogan3PSGreen>(Vec(109, 94), module, Clouds::SIZE_PARAM, 0.0, 1.0, 0.5));
  addParam(createParam<Rogan3PSWhite>(Vec(191, 94), module, Clouds::PITCH_PARAM, -2.0, 2.0, 0.0));

  addParam(createParam<Rogan1PSRed>(Vec(15, 181), module, Clouds::IN_GAIN_PARAM, 0.0, 1.0, 0.5));
  addParam(createParam<Rogan1PSRed>(Vec(82, 181), module, Clouds::DENSITY_PARAM, 0.0, 1.0, 0.5));
  addParam(createParam<Rogan1PSGreen>(Vec(147, 181), module, Clouds::TEXTURE_PARAM, 0.0, 1.0, 0.5));
  addParam(createParam<Rogan1PSWhite>(Vec(214, 181), module, Clouds::BLEND_PARAM, 0.0, 1.0, 0.5));
  
  
  addParam(createParam<Rogan1PSRed>(Vec(214+67, 181), module, Clouds::SPREAD_PARAM, 0.0, 1.0, 0.5));
  addParam(createParam<Rogan1PSGreen>(Vec(214+67+67, 181), module, Clouds::FEEDBACK_PARAM, 0.0, 1.0, 0.5));
  addParam(createParam<Rogan1PSWhite>(Vec(214+67+67+67, 181), module, Clouds::REVERB_PARAM, 0.0, 1.0, 0.5));


  addInput(createInput<PJ3410Port>(Vec(11, 270), module, Clouds::FREEZE_INPUT));
  addInput(createInput<PJ3410Port>(Vec(54, 270), module, Clouds::TRIG_INPUT));
  addInput(createInput<PJ3410Port>(Vec(97, 270), module, Clouds::POSITION_INPUT));
  addInput(createInput<PJ3410Port>(Vec(140, 270), module, Clouds::SIZE_INPUT));
  addInput(createInput<PJ3410Port>(Vec(184, 270), module, Clouds::PITCH_INPUT));
  addInput(createInput<PJ3410Port>(Vec(227, 270), module, Clouds::BLEND_INPUT));

  addInput(createInput<PJ3410Port>(Vec(11, 313), module, Clouds::IN_L_INPUT));
  addInput(createInput<PJ3410Port>(Vec(54, 313), module, Clouds::IN_R_INPUT));
  addInput(createInput<PJ3410Port>(Vec(97, 313), module, Clouds::DENSITY_INPUT));
  addInput(createInput<PJ3410Port>(Vec(140, 313), module, Clouds::TEXTURE_INPUT));
  addOutput(createOutput<PJ3410Port>(Vec(184, 313), module, Clouds::OUT_L_OUTPUT));
  addOutput(createOutput<PJ3410Port>(Vec(227, 313), module, Clouds::OUT_R_OUTPUT));
  
  
  addInput(createInput<PJ3410Port>(Vec(214+67, 291), module, Clouds::SPREAD_INPUT));
  addInput(createInput<PJ3410Port>(Vec(214+67*2, 291), module, Clouds::FEEDBACK_INPUT));
  addInput(createInput<PJ3410Port>(Vec(214+67*3, 291), module, Clouds::REVERB_INPUT));
  
	struct FreezeLight : GreenLight {
		FreezeLight() {
			box.size = Vec(28-16, 28-16);
			bgColor = COLOR_BLACK_TRANSPARENT;
		}
	};

  addParam(createParam<LEDButton>(Vec(68, 52-1), module, Clouds::FREEZE_PARAM, 0.0, 1.0, 0.0));
  addChild(createLight<FreezeLight>(Vec(68+3, 52-1+3), module,Clouds::FREEZE_LIGHT));
#ifdef PARASITES 
  addParam(createParam<LEDButton>(Vec(68-15, 52-1-25), module, Clouds::REVERSE_PARAM, 0.0, 1.0, 0.0));
  addChild(createLight<FreezeLight>(Vec(68+3-15, 52-1+3-25), module, Clouds::REVERSE_LIGHT));
#endif
  
  
}

struct CloudsModeItem : MenuItem {
  Clouds *clouds;
  clouds::PlaybackMode mode;

  void onAction(EventAction &e) override {
    clouds->playbackmode = mode;
  }
  void step() override {
    rightText = (clouds->playbackmode == mode) ? "✔" : "";
  }
};


struct CloudsMonoItem : MenuItem {
  Clouds *clouds;
  bool setting;

  void onAction(EventAction &e) override {
    clouds->mono = setting;
  }
  void step() override {
    rightText = (clouds->mono == setting) ? "✔" : "";
  }
};


struct CloudsLofiItem : MenuItem {
  Clouds *clouds;
  bool setting;

  void onAction(EventAction &e) override {
    clouds->lofi = setting;
  }
  void step() override {
    rightText = (clouds->lofi == setting) ? "✔" : "";
  }
};


struct CloudsBufferItem : MenuItem {
  Clouds *clouds;
  int setting;

  void onAction(EventAction &e) override {
    clouds->buffersize = setting;
  }
  void step() override {
    rightText = (clouds->buffersize == setting) ? "✔" : "";
  }
};

Menu *CloudsWidget::createContextMenu() {
  Menu *menu = ModuleWidget::createContextMenu();

  Clouds *clouds = dynamic_cast<Clouds*>(module);
  assert(clouds);


  menu->pushChild(construct<MenuLabel>());
  menu->pushChild(construct<MenuLabel>(&MenuEntry::text, "MODE"));
  menu->pushChild(construct<CloudsModeItem>(&MenuEntry::text, "GRANULAR", &CloudsModeItem::clouds, clouds, &CloudsModeItem::mode, clouds::PLAYBACK_MODE_GRANULAR));
  menu->pushChild(construct<CloudsModeItem>(&MenuEntry::text, "SPECTRAL", &CloudsModeItem::clouds, clouds, &CloudsModeItem::mode, clouds::PLAYBACK_MODE_SPECTRAL));
  menu->pushChild(construct<CloudsModeItem>(&MenuEntry::text, "LOOPING_DELAY", &CloudsModeItem::clouds, clouds, &CloudsModeItem::mode, clouds::PLAYBACK_MODE_LOOPING_DELAY));
  menu->pushChild(construct<CloudsModeItem>(&MenuEntry::text, "STRETCH", &CloudsModeItem::clouds, clouds, &CloudsModeItem::mode, clouds::PLAYBACK_MODE_STRETCH));
#ifdef PARASITES  
  menu->pushChild(construct<CloudsModeItem>(&MenuEntry::text, "OLIVERB", &CloudsModeItem::clouds, clouds, &CloudsModeItem::mode, clouds::PLAYBACK_MODE_OLIVERB));
  menu->pushChild(construct<CloudsModeItem>(&MenuEntry::text, "RESONESTOR", &CloudsModeItem::clouds, clouds, &CloudsModeItem::mode, clouds::PLAYBACK_MODE_RESONESTOR));
#endif     
  menu->pushChild(construct<MenuLabel>(&MenuEntry::text, "STEREO/MONO"));
  menu->pushChild(construct<CloudsMonoItem>(&MenuEntry::text, "STEREO", &CloudsMonoItem::clouds, clouds, &CloudsMonoItem::setting, false));
  menu->pushChild(construct<CloudsMonoItem>(&MenuEntry::text, "MONO", &CloudsMonoItem::clouds, clouds, &CloudsMonoItem::setting, true));  
  
  menu->pushChild(construct<MenuLabel>(&MenuEntry::text, "HIFI/LOFI"));
  menu->pushChild(construct<CloudsLofiItem>(&MenuEntry::text, "HIFI", &CloudsLofiItem::clouds, clouds, &CloudsLofiItem::setting, false));
  menu->pushChild(construct<CloudsLofiItem>(&MenuEntry::text, "LOFI", &CloudsLofiItem::clouds, clouds, &CloudsLofiItem::setting, true));  
  
#ifdef BUFFERRESIZING
// disable by default as it seems to make alternative modes unstable
  menu->pushChild(construct<MenuLabel>(&MenuEntry::text, "BUFFER SIZE (EXPERIMENTAL)"));
  menu->pushChild(construct<CloudsBufferItem>(&MenuEntry::text, "ORIGINAL", &CloudsBufferItem::clouds, clouds, &CloudsBufferItem::setting, 1));
  menu->pushChild(construct<CloudsBufferItem>(&MenuEntry::text, "2X", &CloudsBufferItem::clouds, clouds, &CloudsBufferItem::setting, 2));
  menu->pushChild(construct<CloudsBufferItem>(&MenuEntry::text, "4X", &CloudsBufferItem::clouds, clouds, &CloudsBufferItem::setting, 4));
  menu->pushChild(construct<CloudsBufferItem>(&MenuEntry::text, "8X", &CloudsBufferItem::clouds, clouds, &CloudsBufferItem::setting, 8));
#endif  
    
  return menu;
}



