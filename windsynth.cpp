#include "daisy_seed.h"

// ReverbSc is part of DaisySP's optional LGPL module set. Keep this in the
// source as well as the Makefile so language servers see the same API.
#ifndef USE_DAISYSP_LGPL
#define USE_DAISYSP_LGPL
#endif

#include "daisysp.h"
#include "dev/oled_ssd130x.h"
#include <stdio.h>

using namespace daisy;
using namespace daisysp;

DaisySeed       hw;
MidiUartHandler midi;
using MyOledDisplay = daisy::OledDisplay<SSD130x4WireSpi128x64Driver>;
MyOledDisplay   display;
dsy_gpio        display_reset_pin;
Encoder         encoder;
Oscillator      osc;
Oscillator      body;
Oscillator      pulse;
Oscillator      overtone;
WhiteNoise      noise;
OnePole         noise_hp;
Svf             filt;
ReverbSc        reverb;
CpuLoadMeter    cpu_load_meter;

float   breath_target = 0.0f;
float   breath_smooth = 0.0f;
float   pitch_bend_target = 0.0f;
float   pitch_bend_smooth = 0.0f;
float   growl_target      = 0.0f;
float   growl_smooth      = 0.0f;
float   growl_phase       = 0.0f;
float   note_target       = 60.0f;
float   note_smooth       = 60.0f;
float   glide_base_ms     = 0.0f;
float   note_glide_coeff  = 1.0f;
float   pwm_phase         = 0.0f;
bool    note_gate     = false;
uint8_t active_note   = 60;
float   reverb_mix    = 0.20f;
float   reverb_time   = 0.85f;
constexpr float master_gain = 2.5f;
constexpr size_t audio_block_size       = 16;
constexpr size_t control_update_interval = 8;
constexpr uint32_t display_idle_timeout_ms = 30000;
constexpr float    reverb_sleep_threshold  = 0.00001f; // -100 dBFS.
float            audio_sample_rate       = 48000.0f;
float            pwm_phase_increment     = 1.05f / 48000.0f;
float            growl_phase_increment   = 30.0f / 48000.0f;
uint32_t         reverb_quiet_sample_limit = 24000;
uint32_t         last_activity_ms         = 0;
bool             display_wake_pending     = false;

enum class EncoderMode
{
	PATCH,
	REVERB_MIX,
	REVERB_TIME
};
EncoderMode encoder_mode = EncoderMode::PATCH;

struct Patch
{
	const char *name;
	float saw_level, triangle_level, pulse_level;
	float output_level, base_cutoff, max_cutoff, breath_curve, key_track;
	float resonance, filter_drive, noise_level, noise_curve;
	float pulse_width, pwm_depth, saturation;
};

// A compact version of Windy1's data-driven patches: each sound stores the
// oscillator, filter, breath and noise settings that give it its character.
const Patch patches[] = {
    {"Dark Reed", .60f, .28f, .50f, .35f, 260.f, 5200.f, 1.72f, .24f,
     .00f, .16f, .005f, 1.45f, .48f, .018f, 1.05f},
    {"Soft Flute", .10f, .82f, .05f, .31f, 520.f, 7600.f, 1.35f, .32f,
     .08f, .08f, .030f, 1.10f, .50f, .006f, 1.00f},
    {"Bright Brass", .90f, .12f, .30f, .30f, 340.f, 10500.f, 1.18f, .38f,
     .18f, .28f, .003f, 1.70f, .46f, .012f, 1.30f},
    {"Hollow Clar", .18f, .20f, .88f, .34f, 180.f, 4100.f, 1.95f, .18f,
     .28f, .12f, .008f, 1.35f, .36f, .010f, 1.08f},
};
constexpr int patch_count = sizeof(patches) / sizeof(patches[0]);
int           patch_index = 0;
const Patch  *current_patch = &patches[0];

constexpr uint32_t settings_offset  = 0x007ff000; // Last 4 KB QSPI sector.
constexpr uint32_t settings_address = 0x90000000 + settings_offset;
constexpr uint32_t settings_magic   = 0x57534e54; // "WSNT"
struct Settings
{
	uint32_t magic;
	uint32_t version;
	uint32_t patch;
	uint32_t patch_inverse;
	uint32_t reverb_mix_percent;
	uint32_t reverb_mix_inverse;
	uint32_t reverb_time_percent;
	uint32_t reverb_time_inverse;
};

void AudioCallback(AudioHandle::InputBuffer,
	               AudioHandle::OutputBuffer,
	               size_t);

bool LoadSettings()
{
	const Settings *saved = reinterpret_cast<const Settings *>(settings_address);
	if(saved->magic != settings_magic || saved->version != 2
	   || saved->patch_inverse != ~saved->patch || saved->patch >= patch_count
	   || saved->reverb_mix_inverse != ~saved->reverb_mix_percent
	   || saved->reverb_mix_percent > 100
	   || saved->reverb_time_inverse != ~saved->reverb_time_percent
	   || saved->reverb_time_percent < 60 || saved->reverb_time_percent > 95)
	{
		return false;
	}
	patch_index  = saved->patch;
	current_patch = &patches[patch_index];
	reverb_mix   = saved->reverb_mix_percent * 0.01f;
	reverb_time  = saved->reverb_time_percent * 0.01f;
	return true;
}

bool SaveSettings()
{
	Settings saved = {settings_magic,
	                  2,
	                  static_cast<uint32_t>(patch_index),
	                  ~static_cast<uint32_t>(patch_index),
	                  static_cast<uint32_t>(reverb_mix * 100.0f + 0.5f),
	                  ~static_cast<uint32_t>(reverb_mix * 100.0f + 0.5f),
	                  static_cast<uint32_t>(reverb_time * 100.0f + 0.5f),
	                  ~static_cast<uint32_t>(reverb_time * 100.0f + 0.5f)};
	// QSPIHandle switches between indirect and memory-mapped modes itself. Audio
	// runs from internal flash, so its DMA callback can continue while the main
	// loop waits for this infrequent erase/write to finish.
	const bool ok = hw.qspi.EraseSector(settings_offset) == QSPIHandle::Result::OK
	          && hw.qspi.Write(settings_address,
	                           sizeof(saved),
	                           reinterpret_cast<uint8_t *>(&saved))
	                 == QSPIHandle::Result::OK;
	return ok;
}

float Clamp(float value, float low, float high)
{
	return value < low ? low : value > high ? high : value;
}

float UpdateOscFreq(float note, float note_offset)
{
	const float freq = mtof(note + note_offset);
	osc.SetFreq(freq);
	body.SetFreq(freq);
	pulse.SetFreq(freq);
	return freq;
}

float BreathCutoff(float breath, float note, const Patch& p)
{
	const float breath_curve     = powf(breath, p.breath_curve);
	const float cutoff
	    = p.base_cutoff * powf(p.max_cutoff / p.base_cutoff, breath_curve);
	const float key_track = powf(2.0f,
	                             (note - 60.0f) * p.key_track / 12.0f);

	return Clamp(cutoff * key_track, 80.0f, 12000.0f);
}

float BreathScoop(float breath)
{
	const float scoop_depth_semitones = -0.18f;
	const float scoop_attain          = 0.35f;
	const float scoop_amount = 1.0f - Clamp(breath / scoop_attain, 0.0f, 1.0f);

	return scoop_depth_semitones * scoop_amount * scoop_amount;
}

float SoftSaturate(float input, float drive)
{
	const float driven = input * drive;
	return driven / (1.0f + fabsf(driven));
}

void MarkActivity()
{
	last_activity_ms     = System::GetNow();
	display_wake_pending = true;
}

void AudioCallback(AudioHandle::InputBuffer in, AudioHandle::OutputBuffer out, size_t size)
{
	cpu_load_meter.OnBlockStart();

	static bool     dsp_sleeping         = false;
	static uint32_t reverb_quiet_samples = 0;
	const bool excitation_requested = note_gate && breath_target > 0.001f;
	if(dsp_sleeping && !excitation_requested)
	{
		for(size_t i = 0; i < size; i++)
		{
			out[0][i] = 0.0f;
			out[1][i] = 0.0f;
		}
		cpu_load_meter.OnBlockEnd();
		return;
	}
	if(dsp_sleeping)
	{
		dsp_sleeping         = false;
		reverb_quiet_samples = 0;
	}

	const Patch *patch = current_patch;
	const float  wet_mix = reverb_mix;
	const float  dry_mix = 1.0f - wet_mix;
	reverb.SetFeedback(reverb_time);

	static const Patch *filter_patch = nullptr;
	if(patch != filter_patch)
	{
		filt.SetRes(patch->resonance);
		filter_patch = patch;
	}

	struct ControlValues
	{
		float amp;
		float noise_amp;
		float body_amp;
		float pulse_amp;
		float saw_amp;
		float overtone_amp;
		float saturation_drive;
	};
	static ControlValues controls = {};
	static size_t        control_countdown = 0;
	float                reverb_block_peak = 0.0f;

	for (size_t i = 0; i < size; i++)
	{
		breath_smooth += 0.0025f * (breath_target - breath_smooth);
		pitch_bend_smooth += 0.01f * (pitch_bend_target - pitch_bend_smooth);
		growl_smooth += 0.0025f * (growl_target - growl_smooth);
		const float note_delta = note_target - note_smooth;
		if(fabsf(note_delta) > 0.0001f)
		{
			note_smooth += note_glide_coeff * note_delta;
		}
		else
		{
			note_smooth = note_target;
		}
		pwm_phase += pwm_phase_increment;
		if(pwm_phase >= 1.0f)
		{
			pwm_phase -= 1.0f;
		}
		growl_phase += growl_phase_increment;
		if(growl_phase >= 1.0f)
		{
			growl_phase -= 1.0f;
		}

		// The nonlinear parameter calculations do not need audio-rate updates.
		// Updating them at 6 kHz preserves smooth modulation while removing most
		// powf/sinf and filter-coefficient work from the hot path.
		if(control_countdown == 0)
		{
			const bool gate = note_gate;
			const float growl_amount = growl_smooth * growl_smooth;
			const float growl_lfo
			    = growl_amount > 0.000001f ? sinf(TWOPI_F * growl_phase) : 0.0f;
			const float growl_mod    = growl_amount * growl_lfo;
			const float overtone_scan
			    = Clamp((growl_smooth - 0.72f) / 0.28f, 0.0f, 1.0f);
			controls.amp
			    = gate ? breath_smooth * patch->output_level
			                 * (1.0f + 0.24f * growl_mod)
			           : 0.0f;
			const float cutoff
			    = Clamp(BreathCutoff(breath_smooth, note_smooth, *patch)
			                * (1.0f + 0.20f * growl_mod),
			            80.0f,
			            12000.0f);
			controls.noise_amp
			    = gate ? patch->noise_level
			                 * powf(breath_smooth, patch->noise_curve)
			           : 0.0f;
			const float low_register
			    = 1.0f - Clamp((note_smooth - 48.0f) / 24.0f, 0.0f, 1.0f);
			controls.body_amp
			    = gate ? patch->triangle_level * (1.0f - 0.12f * breath_smooth)
			                 + 0.08f * low_register
			           : 0.0f;
			controls.pulse_amp
			    = gate ? patch->pulse_level * (0.75f + 0.25f * breath_smooth)
			           : 0.0f;
			const float sax_breath
			    = Clamp((breath_smooth - 0.16f) / 0.66f, 0.0f, 1.2f);
			const float sax_edge
			    = sax_breath
			      * Clamp((note_smooth - 48.0f) / 20.0f, 0.0f, 1.0f);
			controls.saw_amp
			    = gate ? patch->saw_level + 0.14f * sax_edge : 0.0f;
			controls.overtone_amp
			    = gate ? 0.18f * overtone_scan
			                 * (0.70f + 0.30f * breath_smooth)
			           : 0.0f;
			controls.saturation_drive
			    = patch->saturation + 0.05f * (1.0f - breath_smooth)
			      + 0.25f * growl_amount;

			const float fundamental
			    = UpdateOscFreq(note_smooth,
			                    pitch_bend_smooth + BreathScoop(breath_smooth));
			if(overtone_scan > 0.0f)
			{
				// The top of CC1 steps through harmonics 2..8. Limit the
				// selected partial to 45% of the sample rate to avoid aliasing.
				const int requested_harmonic
				    = 2 + static_cast<int>(overtone_scan * 6.999f);
				const int max_harmonic
				    = static_cast<int>(audio_sample_rate * 0.45f / fundamental);
				if(max_harmonic >= 2)
				{
					const int harmonic = requested_harmonic < max_harmonic
					                         ? requested_harmonic
					                         : max_harmonic;
					overtone.SetFreq(fundamental * harmonic);
				}
				else
				{
					controls.overtone_amp = 0.0f;
				}
			}
			const float pwm_lfo = sinf(TWOPI_F * pwm_phase);
			pulse.SetPw(Clamp(patch->pulse_width
			                      + pwm_lfo * patch->pwm_depth * breath_smooth,
			                  0.10f,
			                  0.90f));
			filt.SetFreq(cutoff);
			filt.SetDrive(patch->filter_drive + 0.12f * breath_smooth
			              + 0.10f * growl_amount);
			control_countdown = control_update_interval - 1;
		}
		else
		{
			--control_countdown;
		}

		const float exciter = osc.Process() * controls.saw_amp
		                      + body.Process() * controls.body_amp
		                      + pulse.Process() * controls.pulse_amp
		                      + (controls.overtone_amp > 0.000001f
		                             ? overtone.Process() * controls.overtone_amp
		                             : 0.0f);
		const float air
		    = noise_hp.Process(noise.Process()) * controls.noise_amp;

		filt.Process(SoftSaturate(exciter, controls.saturation_drive));

		const float dry = (filt.Low() + air) * controls.amp;
		float       wet_left, wet_right;
		reverb.Process(dry, dry, &wet_left, &wet_right);
		const float signal_peak
		    = fmaxf(fabsf(dry), fmaxf(fabsf(wet_left), fabsf(wet_right)));
		if(signal_peak > reverb_block_peak)
		{
			reverb_block_peak = signal_peak;
		}
		const float mixed_left  = dry * dry_mix + wet_left * wet_mix;
		const float mixed_right = dry * dry_mix + wet_right * wet_mix;
		out[0][i] = Clamp(mixed_left * master_gain, -1.0f, 1.0f);
		out[1][i] = Clamp(mixed_right * master_gain, -1.0f, 1.0f);
	}
	if(excitation_requested || reverb_block_peak >= reverb_sleep_threshold)
	{
		reverb_quiet_samples = 0;
	}
	else if(reverb_quiet_samples < reverb_quiet_sample_limit)
	{
		reverb_quiet_samples += size;
		if(reverb_quiet_samples >= reverb_quiet_sample_limit)
		{
			dsp_sleeping = true;
		}
	}

	cpu_load_meter.OnBlockEnd();
}

void HandleMidiMessage(MidiEvent m)
{
	switch(m.type)
	{
		case NoteOn:
		{
			MarkActivity();
			NoteOnEvent p = m.AsNoteOn();
			if(p.velocity == 0)
			{
				if(p.note == active_note)
				{
					note_gate = false;
				}
			}
			else
			{
				const bool  legato        = note_gate;
				const float previous_note = note_smooth;

				active_note = p.note;
				note_target = p.note;
				if(!legato || glide_base_ms <= 0.0f)
				{
					note_smooth = note_target;
					note_glide_coeff = 1.0f;
				}
				else
				{
					const float interval = fabsf(note_target - previous_note);
					const float time_per_semitone_ms = glide_base_ms / 12.0f;
					const float glide_samples
					    = audio_sample_rate * time_per_semitone_ms * interval
					      * 0.001f;
					note_glide_coeff
					    = glide_samples <= 1.0f ? 1.0f : 1.0f / glide_samples;
				}
				note_gate = true;
			}
		}
		break;

		case NoteOff:
		{
			MarkActivity();
			NoteOffEvent p = m.AsNoteOff();
			if(p.note == active_note)
			{
				note_gate = false;
			}
		}
		break;

		case ControlChange:
		{
			ControlChangeEvent p = m.AsControlChange();
			if(p.control_number == 1)
			{
				const float growl = p.value / 127.0f;
				if(fabsf(growl_target - growl) > 0.001f)
				{
					MarkActivity();
				}
				growl_target = growl;
			}
			else if(p.control_number == 2)
			{
				if(fabsf(breath_target - p.value / 127.0f) > 0.001f)
				{
					MarkActivity();
				}
				breath_target = p.value / 127.0f;
			}
			else if(p.control_number == 5)
			{
				MarkActivity();
				const float porta = p.value / 127.0f;
				glide_base_ms    = powf(porta, 4.0f) * 2500.0f;
			}
		}
		break;

		case PitchBend:
		{
			PitchBendEvent p = m.AsPitchBend();
			const float bend = (p.value / 8192.0f) * 2.0f;
			if(fabsf(pitch_bend_target - bend) > 0.001f)
			{
				MarkActivity();
			}
			pitch_bend_target = bend;
		}
		break;

		default: break;
	}
}

int main(void)
{
	hw.Init();
	hw.SetAudioBlockSize(audio_block_size);
	hw.SetAudioSampleRate(SaiHandle::Config::SampleRate::SAI_48KHZ);
	LoadSettings();
	// Encoder A/B/switch: Seed D0, D1 and D2. Click switches between patch
	// selection and reverb mix adjustment.
	encoder.Init(hw.GetPin(0), hw.GetPin(1), hw.GetPin(2));

	MyOledDisplay::Config display_config;
	display.Init(display_config);
	display_reset_pin.pin
	    = display_config.driver_config.transport_config.pin_config.reset;
	display.Fill(false);
	display.SetCursor(0, 0);
	display.WriteString(current_patch->name, Font_11x18, true);
	display.Update();

	const float samplerate = hw.AudioSampleRate();
	audio_sample_rate       = samplerate;
	pwm_phase_increment     = 1.05f / samplerate;
	growl_phase_increment   = 30.0f / samplerate;
	reverb_quiet_sample_limit = static_cast<uint32_t>(samplerate * 0.5f);
	cpu_load_meter.Init(samplerate, hw.AudioBlockSize());
	osc.Init(samplerate);
	osc.SetWaveform(Oscillator::WAVE_POLYBLEP_SAW);
	osc.SetAmp(1.0f);
	UpdateOscFreq(note_smooth, 0.0f);

	body.Init(samplerate);
	body.SetWaveform(Oscillator::WAVE_POLYBLEP_TRI);
	body.SetAmp(1.0f);
	body.SetFreq(mtof(note_smooth));

	pulse.Init(samplerate);
	pulse.SetWaveform(Oscillator::WAVE_POLYBLEP_SQUARE);
	pulse.SetAmp(1.0f);
	pulse.SetPw(0.48f);
	pulse.SetFreq(mtof(note_smooth));

	overtone.Init(samplerate);
	overtone.SetWaveform(Oscillator::WAVE_SIN);
	overtone.SetAmp(1.0f);
	overtone.SetFreq(mtof(note_smooth) * 2.0f);

	noise.Init();
	noise.SetAmp(1.0f);

	noise_hp.Init();
	noise_hp.SetFilterMode(OnePole::FILTER_MODE_HIGH_PASS);
	noise_hp.SetFrequency(3500.0f / samplerate);

	filt.Init(samplerate);
	filt.SetFreq(BreathCutoff(0.0f, note_smooth, *current_patch));
	filt.SetRes(0.0f);
	filt.SetDrive(0.16f);

	reverb.Init(samplerate);
	reverb.SetFeedback(reverb_time);
	reverb.SetLpFreq(10000.0f);

	MidiUartHandler::Config midi_config;
	midi_config.transport_config.periph
	    = UartHandler::Config::Peripheral::USART_1;
	midi_config.transport_config.rx = {DSY_GPIOB, 7}; // Seed D14
	midi_config.transport_config.tx = {DSY_GPIOX, 0}; // TX unused
	midi.Init(midi_config);
	midi.StartReceive();

	hw.StartAudio(AudioCallback);
	uint32_t last_display_update = 0;
	uint32_t settings_changed_at   = 0;
	bool     settings_save_pending = false;
	bool     display_awake         = true;
	last_activity_ms               = System::GetNow();

	while(1)
	{
		encoder.Debounce();
		if(encoder.RisingEdge())
		{
			MarkActivity();
			encoder_mode = encoder_mode == EncoderMode::PATCH
			                   ? EncoderMode::REVERB_MIX
			                   : encoder_mode == EncoderMode::REVERB_MIX
			                         ? EncoderMode::REVERB_TIME
			                         : EncoderMode::PATCH;
			last_display_update = 0;
		}
		const int32_t increment = encoder.Increment();
		// This encoder's wiring reports clockwise turns as negative increments.
		const bool increase = increment < 0;
		if(increment != 0)
		{
			MarkActivity();
			if(encoder_mode == EncoderMode::PATCH)
			{
				patch_index = (patch_index
				               + (increase ? 1 : patch_count - 1))
				              % patch_count;
				current_patch     = &patches[patch_index];
				settings_changed_at   = System::GetNow();
				settings_save_pending = true;
			}
			else if(encoder_mode == EncoderMode::REVERB_MIX)
			{
				reverb_mix = Clamp(reverb_mix + (increase ? 0.05f : -0.05f),
				                   0.0f,
				                   1.0f);
				settings_changed_at   = System::GetNow();
				settings_save_pending = true;
			}
			else
			{
				reverb_time = Clamp(reverb_time + (increase ? 0.05f : -0.05f),
				                    0.60f,
				                    0.95f);
				settings_changed_at   = System::GetNow();
				settings_save_pending = true;
			}
			last_display_update = 0;
		}
		midi.Listen();
		while(midi.HasEvents())
		{
			HandleMidiMessage(midi.PopEvent());
		}

		const uint32_t now = System::GetNow();
		if(display_wake_pending)
		{
			display_wake_pending = false;
			if(!display_awake)
			{
				display.Init(display_config);
				display_awake       = true;
				last_display_update = 0;
			}
		}
		if(display_awake && !note_gate && breath_target <= 0.01f
		   && now - last_activity_ms >= display_idle_timeout_ms)
		{
			display.Fill(false);
			display.Update();
			// Holding reset low turns the panel and its charge pump off. The
			// next meaningful input runs the normal initialization sequence.
			dsy_gpio_write(&display_reset_pin, 0);
			display_awake = false;
		}
		if(settings_save_pending && now - settings_changed_at >= 10000)
		{
			settings_save_pending = !SaveSettings();
			if(settings_save_pending)
				settings_changed_at = now; // Retry in 10 seconds after a flash error.
		}
		if(display_awake && now - last_display_update >= 50)
		{
			last_display_update = now;
			char line[24];
			display.Fill(false);
			display.SetCursor(0, 0);
			display.WriteString(current_patch->name, Font_11x18, true);
			display.SetCursor(0, 26);
			snprintf(line,
			         sizeof(line),
			         "%cRev.Mix: %3u%%",
			         encoder_mode == EncoderMode::REVERB_MIX ? '>' : ' ',
			         static_cast<unsigned>(reverb_mix * 100.0f + 0.5f));
			display.WriteString(line, Font_7x10, true);
			display.SetCursor(0, 38);
			snprintf(line,
			         sizeof(line),
			         "%cRev.Time:%3u%%",
			         encoder_mode == EncoderMode::REVERB_TIME ? '>' : ' ',
			         static_cast<unsigned>(reverb_time * 100.0f + 0.5f));
			display.WriteString(line, Font_7x10, true);
			const uint_fast8_t breath_width
			    = static_cast<uint_fast8_t>(breath_target * 127.0f);
			if(breath_width > 0)
			{
				display.DrawRect(0, 62, breath_width, 63, true, true);
			}
			const float average_load = cpu_load_meter.GetAvgCpuLoad();
			const float peak_load    = cpu_load_meter.GetMaxCpuLoad();
			const unsigned average_percent
			    = isfinite(average_load)
			          ? static_cast<unsigned>(Clamp(average_load * 100.0f,
			                                        0.0f,
			                                        999.0f)
			                                  + 0.5f)
			          : 0;
			const unsigned peak_percent
			    = isfinite(peak_load)
			          ? static_cast<unsigned>(Clamp(peak_load * 100.0f,
			                                        0.0f,
			                                        999.0f)
			                                  + 0.5f)
			          : 0;
			snprintf(line,
			         sizeof(line),
			         "CPU:%3u%% P:%3u%%",
			         average_percent,
			         peak_percent);
			display.SetCursor(0, 50);
			display.WriteString(line, Font_7x10, true);
			display.Update();
		}
	}
}
