#include "midi/inc/MidiFile.h"
#include "midi/inc/Options.h"
#include <iostream>
#include <vector>
#include <string>
#include <fstream>
#include <stdio.h>
using namespace std;

// TODO:
//     + Combine coarse and fine pitch so that both are updated to reflect
//       the source pitch bend range, use note splitting
//	   + Add non-coincidental event removal
//     + Add event compression
//     + Add layering compression
//     + Determine root pitch for note offset (NOTE_BIAS)
//     + Determine m64 volume scaling, definitely sounds wrong
//     + Add UI  

#ifndef _NDEBUG
#define DEBUG_MIDI_FILE "smrpgtest.mid"
#endif

#define NOTE_BIAS 24

unsigned short bit_mask_from_value[16] =
	{	0x0001, 0x0003, 0x0007, 0x000F,
		0x001F, 0x003F, 0x007F, 0x00FF,
		0x01FF, 0x03FF, 0x07FF, 0x0FFF,
		0x1FFF, 0x3FFF, 0x7FFF, 0xFFFF};

short rev_short(short _x)
{
	return ((unsigned short)_x >> 8) | (_x << 8);
}

enum class ControllerSourceType
{
	CoarsePitch,
	FinePitch, 
	Volume, 
	Pan, 
	Tempo, 
	Unknown, 
	UserFixed
};

enum class NoteType
{
	Note,
	Rest
};
class NoteEvent
{
public:
	NoteEvent(NoteType _type,
		int _ticks, 
		float _velocity = 0, 
		unsigned char _value = 0)
	{
		type = _type;
		ticks = _ticks;
		velocity = _velocity;
		note = _value;
	}
	NoteType type;
	int ticks;
	unsigned char note;
	float velocity;
};

class ControllerEvent
{
public:
	ControllerEvent(int _ticks, float _value)
	{
		value = _value;
		ticks = _ticks;
	}
	int ticks;
	float value;
};

class ControllerSource
{
public:
	ControllerSource()
	{
		clear();
	}
	void clear()
	{
		type = ControllerSourceType::Unknown;
		owner_track_id = -1;
		base_value = 0.0;
		multiplier = 1.0;
		events.clear();
		controller_number = -1;
	}
	void convert_clock_base(int _from_base, int _total_ticks)
	{
		float divisor;
		int i;
		int d_duration;
		int prev_duration;
		divisor = 48.0f / (float)_from_base;
		i = 0;
		while(i < events.size())
		{
			if (i < (events.size() - 1))
			{
				d_duration = (events[i + 1].ticks - events[i].ticks)*divisor;
			}
			else
			{
				d_duration = (_total_ticks - events[i].ticks)*divisor;
			}
			events[i].ticks *= divisor;
			if (i > 0)
			{
				if (events[i].ticks == events[i - 1].ticks)
				{
					if (d_duration > prev_duration)
					{
						events.erase(events.begin() + i - 1);
						prev_duration = d_duration;
					}
					else
					{
						events.erase(events.begin() + i);
					}
				}
				else
				{
					prev_duration = d_duration;
					i++;
				}
			}
			else
			{
				prev_duration = d_duration;
				i++;
			}
		}
	}
	float get(int _index)
	{
		float v;
		v = events[_index].value * multiplier + base_value;
		if (v > 1.0)
		{
			v = 1.0;
		} 
		else if (v < 0.0)
		{
			v = 0.0;
		}
		return v;
	}
	float base_value; 
	float multiplier; 
	vector<ControllerEvent> events;
	ControllerSourceType type;
	int controller_number;
	int owner_track_id;
};

#define PARAM_SOURCE_NONE -1
class Track
{
public:
	Track()
	{
		clear();
	}
	void clear()
	{
		notes.clear();
		name = "";
		coarse_pitch_source = PARAM_SOURCE_NONE;
		fine_pitch_source = PARAM_SOURCE_NONE;
		volume_source = PARAM_SOURCE_NONE;
		pan_source = PARAM_SOURCE_NONE;
		echo_source = PARAM_SOURCE_NONE;
		vibrato_source = PARAM_SOURCE_NONE;
		instrument = 0;
		velocity_multiplier = 1.0;
	}
	void convert_clock_base(int _from_base, int _total_ticks)
	{
		float divisor;
		int i;
		int d_duration;
		int prev_duration;
		divisor = 48.0f / (float)_from_base;
		i = 0;
		while (i < notes.size())
		{
			if (i < (notes.size() - 1))
			{
				d_duration = (notes[i + 1].ticks - notes[i].ticks)*divisor;
			}
			else
			{
				d_duration = (_total_ticks - notes[i].ticks)*divisor;
			}
			notes[i].ticks *= divisor;
			if (i > 0)
			{
				if (notes[i].ticks == notes[i - 1].ticks)
				{
					if (((d_duration > prev_duration) || 
						((notes[i].type == NoteType::Note) && 
							(notes[i - 1].type == NoteType::Rest))) && 
						!((notes[i].type == NoteType::Rest) && 
							(notes[i - 1].type == NoteType::Note)))
					{
						notes.erase(notes.begin() + i - 1);
						prev_duration = d_duration;
					}
					else
					{
						notes.erase(notes.begin() + i);
					}
				}
				else
				{
					prev_duration = d_duration;
					i++;
				}
			}
			else
			{
				prev_duration = d_duration;
				i++;
			}
		}
	}
	
	unsigned char instrument; 
	string name; 
	vector<NoteEvent> notes;
	int coarse_pitch_source;
	int fine_pitch_source;
	int volume_source;
	int pan_source;
	int echo_source;
	int vibrato_source;
	float velocity_multiplier;
};

class Sequence
{
public:
	Sequence()
	{
		tempo_source = PARAM_SOURCE_NONE;
		source_vibrato_range = 4;
		source_fine_pitch_range = 12;
		bank = 0;
		volume = 1.0;
	}
	void trim_events()
	{
		int i;
		int j;
		for (i = 0; i < sources.size(); i++)
		{
			j = sources[i].events.size() - 1;
			while(sources[i].events[j].ticks >= total_ticks)
			{
				sources[i].events.pop_back();
				j--;
				if (j < 0) break;
			}
		}
	}
	void convert_clock_base()
	{
		int i;
		for (i = 0; i < sources.size(); i++)
		{
			sources[i].convert_clock_base(ticks_per_quarter, total_ticks);
		}
		for (i = 0; i < tracks.size(); i++)
		{
			tracks[i].convert_clock_base(ticks_per_quarter, total_ticks);
		}
		total_ticks *= 48.0f / (float)ticks_per_quarter;
		ticks_per_quarter = 48;
	}
	class EventStream
	{
	public:
		EventStream(
			ControllerSource* _event_source,
			unsigned char _event_code,
			float _multiplier,
			float _offset)
		{
			cur_event = 0;
			event_source = _event_source;
			event_code = _event_code;
			multiplier = _multiplier;
			offset = _offset;
		}
		int cur_event;
		ControllerSource* event_source;
		unsigned char event_code;
		float multiplier;
		float offset;
	};

	std::vector<uchar> create_m64()
	{
#define ADD(_X_) m64.push_back(_X_)
#define ADD_W(_X_)							\
	m64.push_back(((_X_) >> 8) & 0xFF);		\
	m64.push_back((_X_) & 0xFF)
#define ADD_V(_X_)							\
	{										\
		if((_X_) < 127)						\
		{									\
			ADD((_X_));						\
		}									\
		else								\
		{									\
			ADD_W((_X_) | 0x8000);			\
		}									\
	}
		vector<uchar> m64;
		vector<int> track_pointers;
		vector<int> note_pointers;
		vector<EventStream> events;
		int i;
		int j;
		int last_tick;
		int tick;
		int near_event;
		float value;
		int val_int;
		int cur_note_group;
		int note_group;
		int note;
		int note_fmt;
		int prev_duration;
		int this_duration;
		int mode;
		int this_and_next_duration;
		float play_percentage;
		bool next_note_is_rest;
		float fine_pitch_scaling;
		float vibrato_scaling;
		float note_vel;
		bool delta_time_event;

		fine_pitch_scaling = source_fine_pitch_range / 12.0;
		vibrato_scaling = source_vibrato_range / 12.0;

		ADD(0xD3);									
		ADD((unsigned char)bank);					
		ADD(0xD7);									
		ADD_W(bit_mask_from_value[tracks.size() - 1]);	
													
		for (i = 0; i < tracks.size(); i++)			
		{
			ADD(0x90 | i);
			ADD_W(0x0000);
			track_pointers.push_back(m64.size() - 2);
		}
		ADD(0xDB);									
		ADD(volume * 100.0); // NO CLUE WHAT THIS NUMBER ACTUALLY IS

		if (tempo_source == PARAM_SOURCE_NONE)		
		{											
			ADD(0xDD);								
			ADD(0x78);		
			ADD(0xFD);
			ADD_V(total_ticks);
		}
		else
		{
			delta_time_event = false;
			last_tick = 0;
			for (i = 0; 
				i < sources[tempo_source].events.size(); 
				i++)
			{
				tick = sources[tempo_source].events[i].ticks;
				if (tick > 0)
				{
					delta_time_event = true;
					ADD(0xFD);
					ADD_V(tick - last_tick);
				}
				ADD(0xDD);
				ADD((uchar)(sources[tempo_source].get(i) * 255.0));
				last_tick = tick;
			}
			if (!delta_time_event)
			{
				ADD(0xFD);
				ADD_V(total_ticks);
			}
		}
		ADD(0xFF);

		for (i = 0; i < tracks.size(); i++)
		{
			ADD(0xC4);
			*((short*) &(m64[track_pointers[i]])) = rev_short(m64.size() - 1);
			ADD(0x90);
			ADD_W(0x0000);
			note_pointers.push_back(m64.size() - 2);
			ADD(0xC1);
			ADD(tracks[i].instrument);
			events.clear();
			if (tracks[i].coarse_pitch_source == PARAM_SOURCE_NONE)
			{
				ADD(0xC2);
				ADD(0x00);
			}
			else
			{
				events.push_back(
					EventStream(
						&sources[tracks[i].coarse_pitch_source], 
						0xC2, 
						255, -128)
					);
			}
			if (tracks[i].echo_source == PARAM_SOURCE_NONE)
			{
				ADD(0xD4);
				ADD(0x00);
			}
			else
			{
				events.push_back(
					EventStream(
						&sources[tracks[i].echo_source],
						0xD4,
						255, 0)
					);
			}
			if (tracks[i].fine_pitch_source == PARAM_SOURCE_NONE)
			{
				ADD(0xD3);
				ADD(0x00);
			}
			else
			{
				events.push_back(
					EventStream(
						&sources[tracks[i].fine_pitch_source],
						0xD3,
						255.0*fine_pitch_scaling, -128.0*fine_pitch_scaling)
					);
			}
			if (tracks[i].pan_source == PARAM_SOURCE_NONE)
			{
				ADD(0xDD);
				ADD(0x40);
			}
			else
			{
				events.push_back(
					EventStream(
						&sources[tracks[i].pan_source],
						0xDD,
						126, 1)
					);
			}
			if (tracks[i].vibrato_source == PARAM_SOURCE_NONE)
			{
				ADD(0xD8);
				ADD(0x00);
			}
			else
			{
				events.push_back(
					EventStream(
						&sources[tracks[i].vibrato_source],
						0xD8,
						255.0*vibrato_scaling, 1)
					);
			}
			if (tracks[i].volume_source == PARAM_SOURCE_NONE)
			{
				ADD(0xDF);
				ADD(0xC4);
			}
			else
			{
				events.push_back(
					EventStream(
						&sources[tracks[i].volume_source],
						0xDF,
						128, 0) // NO CLUE WHAT THIS NUMBER ACTUALLY IS
					);
			}
			last_tick = 0;
			delta_time_event = false;
			while (!events.empty())
			{
				near_event = 0;
				tick = (*(events[0].event_source)).events[
					events[0].cur_event].ticks;
				for (j = 1; j < events.size(); j++)
				{
					if ((*(events[j].event_source)).events[
						events[j].cur_event].ticks < tick)
					{
						tick = (*(events[j].event_source)).events[
							events[j].cur_event].ticks;
						near_event = j;
					}
				}

				value = (*(events[near_event].event_source)).get(
					events[near_event].cur_event);
				val_int = (int)(value * events[near_event].multiplier +
					events[near_event].offset);

				if (tick != last_tick)
				{
					delta_time_event = true;
					ADD(0xFD);
					ADD_V(tick - last_tick);
				}
				ADD(events[near_event].event_code);
				ADD(val_int);
				events[near_event].cur_event++;
				if (events[near_event].event_source->events.size() ==
					events[near_event].cur_event)
				{
					events.erase(events.begin() + near_event);
				}
				last_tick = tick;
			} 
			if (!delta_time_event)
			{
				ADD(0xFD);
				ADD_V(total_ticks);
			}
			ADD(0xFF);
		}
		for (i = 0; i < tracks.size(); i++)
		{
			*((short*)&(m64[note_pointers[i]])) = rev_short(m64.size());
			j = 0;
			cur_note_group = 0;
			prev_duration = 0;
			while (j < tracks[i].notes.size())
			{
				if (tracks[i].notes[j].type == NoteType::Rest)
				{
					ADD(0xC0);
					if (j == (tracks[i].notes.size() - 1))
					{
						ADD_V(total_ticks - tracks[i].notes[j].ticks);
					}
					else
					{
						ADD_V(tracks[i].notes[j + 1].ticks -
							tracks[i].notes[j].ticks);
					}
					j += 1;
				}
				else if (tracks[i].notes[j].type == NoteType::Note)
				{
					note = tracks[i].notes[j].note;
					note_group = cur_note_group;
					while ((note - (note_group * 64 + NOTE_BIAS)) < 0)
					{
						note_group--;
					}
					while ((note - (note_group * 64 + NOTE_BIAS)) >= 64)
					{
						note_group++;
					}
					if (note_group != cur_note_group)
					{
						ADD(0xC2);
						ADD(note_group * 64);
						cur_note_group = note_group;
					}

					if (j == (tracks[i].notes.size() - 1))
					{
						next_note_is_rest = false;
						this_duration = total_ticks - tracks[i].notes[j].ticks;
					}
					else
					{
						this_duration = tracks[i].notes[j + 1].ticks - 
							tracks[i].notes[j].ticks;
						if (j == (tracks[i].notes.size() - 2))
						{
							this_and_next_duration = total_ticks - 
								tracks[i].notes[j].ticks;
						}
						else
						{
							this_and_next_duration = tracks[i].notes[
									j + 2
								].ticks - tracks[i].notes[j].ticks;
						}
						if (tracks[i].notes[j + 1].type == NoteType::Rest)
						{
							next_note_is_rest = true;
						}
						else
						{
							next_note_is_rest = false;
						}
					}
					
					if (next_note_is_rest)
					{
						if (this_and_next_duration <= 255)
						{
							if (this_and_next_duration == prev_duration)
							{
								mode = 3;
							}
							else
							{
								mode = 1;
							}
						}
						else
						{
							mode = 2;
						}
					}
					else
					{
						mode = 2;
					}

					note_fmt = note - (cur_note_group * 64 + NOTE_BIAS);
					switch (mode)
					{
					case 1:
						ADD(note_fmt);
						ADD_V(this_and_next_duration);
						prev_duration = this_and_next_duration;
						note_vel = tracks[i].notes[j].velocity *
							tracks[i].velocity_multiplier;
						if (note_vel > 1.0)
						{
							note_vel = 1.0;
						}
						else if (note_vel < 0.0)
						{
							note_vel = 0.0;
						}
						ADD(note_vel * 100.0);
						play_percentage = ((float) (this_and_next_duration - 
							this_duration)) / 
								((float) this_and_next_duration) * 255.0;
						ADD(play_percentage);
						j += 2;
						break;
					case 2:
						ADD(64 + note_fmt);
						ADD_V(this_duration);
						prev_duration = this_duration;
						note_vel = tracks[i].notes[j].velocity *
							tracks[i].velocity_multiplier;
						if (note_vel > 1.0)
						{
							note_vel = 1.0;
						}
						else if (note_vel < 0.0)
						{
							note_vel = 0.0;
						}
						ADD(note_vel * 100.0);
						j += 1;
						break;
					case 3:
						ADD(128 + note_fmt);
						note_vel = tracks[i].notes[j].velocity *
							tracks[i].velocity_multiplier;
						if (note_vel > 1.0)
						{
							note_vel = 1.0;
						}
						else if (note_vel < 0.0)
						{
							note_vel = 0.0;
						}
						ADD(note_vel * 100.0);
						play_percentage = ((float)(this_and_next_duration -
							this_duration)) /
							((float)this_and_next_duration) * 255.0;
						ADD(play_percentage);
						j += 2;
					}
				}
			}
		}
		return m64;
	}
	int tempo_source; 
	float source_vibrato_range;   
	float source_fine_pitch_range; 
	vector<ControllerSource> sources; 
	vector<Track> tracks;
	uint32_t ticks_per_quarter; 
	int total_ticks;
	unsigned char bank;
	float volume;
};

int get_source_index(vector<ControllerSource>& _sources,
	int _track,
	ControllerSourceType _type,
	int _controller_number = -1)
{
	int source_index;
	int i;
	source_index = -1;
	for (i = 0; i < (int)_sources.size(); i++)
	{
		if (((_sources[i].type == _type) && (_controller_number == -1)) ||
			((_sources[i].type == ControllerSourceType::Unknown) &&
				(_sources[i].controller_number == _controller_number)))
		{
			source_index = i;
			break;
		}
	}
	if (source_index == -1)
	{
		_sources.push_back(ControllerSource());
		source_index = _sources.size() - 1;
		_sources[source_index].type = _type;
		_sources[source_index].owner_track_id = _track;
		_sources[source_index].controller_number = _controller_number;
	}
	return source_index;
}



void press_enter_to_continue()
{
	std::cout << "Press ENTER to continue... " << flush;
	std::cin.ignore(std::numeric_limits <std::streamsize> ::max(), '\n');
}


int main(int _argc, char** _argv)
{
	string filename;
	string out_filename;
	int cur_track;
	int cur_event;
	int i;
	Sequence seq;
	Track new_track;
	int ticks;
	int source_index;
	int shift_reg;
	int last_note_ending_ticks;
	vector<ControllerSource> cur_sources;
	size_t previous_size;
	vector<uchar> m64;
	fstream output;
	MidiFile midifile;
#ifdef _NDEBUG
	Options options;
#endif

#ifdef _NDEBUG
	options.process(_argc, _argv);
	if (options.getArgCount() != 1) 
	{
		cerr << "You must specify exactly one MIDI file.\n";
		return 1;
	}
	filename = options.getArg(1);
#else
	filename = DEBUG_MIDI_FILE;
#endif

	midifile.read(filename);
	if (!midifile.status())
	{
		cerr << "Error reading MIDI file " << filename << endl;
		return 1;
	}

	midifile.linkNotePairs();
	midifile.absoluteTicks();
	midifile.sortTracks();
	seq.ticks_per_quarter = midifile.getTicksPerQuarterNote();
	seq.total_ticks = midifile.getTotalTimeInTicks();

	for (cur_track = 0; cur_track < midifile.getNumTracks(); cur_track++)
	{
		new_track.clear();
		cur_sources.clear();
		last_note_ending_ticks = 0;
		for (cur_event = 0;
			cur_event < midifile[cur_track].getSize();
			cur_event++)
		{
			ticks = midifile[cur_track][cur_event].tick;
			switch (midifile[cur_track][cur_event][0] & 0xF0)
			{
			case 0xE0:
				if (ticks < seq.total_ticks)
				{
					source_index =
						get_source_index(cur_sources,
							cur_track,
							ControllerSourceType::FinePitch);
					shift_reg = (((int)midifile[cur_track][cur_event][1]) |
						((int)midifile[cur_track][cur_event][2]) << 7);
					cur_sources[source_index].events.push_back(
						ControllerEvent(ticks, (float)shift_reg / 16383.0)
						);
				}
				break;
			case 0xB0:
				if (ticks < seq.total_ticks)
				{
					switch (midifile[cur_track][cur_event][1])
					{
					case 0x07:
						source_index = get_source_index(cur_sources,
							cur_track,
							ControllerSourceType::Volume);
						break;
					case 0x0A:
						source_index = get_source_index(cur_sources,
							cur_track,
							ControllerSourceType::Pan);
						break;
					default:
						source_index = get_source_index(cur_sources,
							cur_track,
							ControllerSourceType::Unknown,
							midifile[cur_track][cur_event][1]);
					}
					cur_sources[source_index].events.push_back(
						ControllerEvent(ticks,
							(float)midifile[cur_track][cur_event][2] / 127.0f)
						);
				}
				break;
			case 0xF0:
				switch(midifile[cur_track][cur_event][1])
				{
				case 0x51:
					if (ticks < seq.total_ticks)
					{
						shift_reg = 0;
						for (i = 0;
						i < (int)midifile[cur_track][cur_event][2];
							i++)
						{
							shift_reg = (shift_reg << 8) |
								((int)midifile[cur_track][cur_event][3 + i]);
						}
						shift_reg = (int)
							((60000000.0 / ((float)shift_reg)) + 0.5);
						source_index = get_source_index(cur_sources,
							cur_track,
							ControllerSourceType::Tempo);
						cur_sources[source_index].events.push_back(
							ControllerEvent(ticks, (float)shift_reg / 255.0f)
							);
					}
					break;
				case 0x03:
					new_track.name = "";
					for (i = 0;
						i < (int)midifile[cur_track][cur_event][2];
						i++)
					{
						new_track.name += 
							midifile[cur_track][cur_event][3 + i];
					}
					break;
				case 0x2F:
					if ((!new_track.notes.empty()) &&
						(ticks > last_note_ending_ticks))
					{
						new_track.notes.push_back(
							NoteEvent(NoteType::Rest, last_note_ending_ticks)
							);
					}
				}
				break;
			case 0x90:
				if (ticks > last_note_ending_ticks)
				{
					new_track.notes.push_back(
						NoteEvent(NoteType::Rest,
							last_note_ending_ticks)
						);
				}
				last_note_ending_ticks =
					midifile[cur_track][cur_event].getLinkedEvent()->tick;
				new_track.notes.push_back(
					NoteEvent(NoteType::Note,
						ticks,
						(float)midifile[cur_track][cur_event][2] / 127.0f,
						midifile[cur_track][cur_event][1])
					);
				break;
			}
		}
		previous_size = seq.sources.size();
		seq.sources.insert(seq.sources.end(), 
			cur_sources.begin(), 
			cur_sources.end());
		if (!new_track.notes.empty())
		{
			for (i = previous_size; i < seq.sources.size(); i++)
			{
				switch (seq.sources[i].type)
				{
				case ControllerSourceType::FinePitch:
					new_track.fine_pitch_source = i;
					break;
				case ControllerSourceType::Pan:
					new_track.pan_source = i;
					break;
				case ControllerSourceType::Volume:
					new_track.volume_source = i;
				}
			}
			new_track.instrument = seq.tracks.size();
			seq.tracks.push_back(new_track);
		}
	}
	for (i = 0; i < seq.sources.size(); i++)
	{
		if (seq.sources[i].type == ControllerSourceType::Tempo)
		{
			seq.tempo_source = i;
			break;
		}
	}
	seq.convert_clock_base();
	seq.trim_events();

	// DEMO TRACK STUFF
	
	seq.bank = 23;

	seq.tracks[0].instrument = 4;

	cout << seq.tracks.size() << endl;

	press_enter_to_continue();

	/*
	seq.sources[seq.tracks[0].fine_pitch_source].base_value = -(0.2 / 12.0) * 0.5;
	seq.sources[seq.tracks[0].volume_source].multiplier = 1.4;
	seq.sources[seq.tracks[0].volume_source].base_value = -0.4;
	seq.sources[seq.tracks[0].pan_source].base_value = 0.09;

	seq.sources[seq.tracks[1].volume_source].multiplier = 1.2;

	seq.sources[seq.tracks[2].pan_source].base_value = -0.09;
	seq.sources[seq.tracks[2].fine_pitch_source].base_value = 0.352;
	seq.sources[seq.tracks[3].fine_pitch_source].base_value = -0.2;
	seq.sources[seq.tracks[3].volume_source].base_value = 0.5;
	seq.sources[seq.tracks[2].volume_source].base_value = -0.4;

	seq.tracks[3].velocity_multiplier = 1.1;

	seq.sources[seq.tracks[4].fine_pitch_source].base_value = -0.3 / 12.0;
	seq.sources[seq.tracks[5].fine_pitch_source].base_value = -0.3 / 12.0;
	seq.sources[seq.tracks[6].fine_pitch_source].base_value = -0.3 / 12.0;

	seq.sources[seq.tracks[7].fine_pitch_source].base_value = (-3.2 / 12.0) * 0.5;
	seq.sources[seq.tracks[8].fine_pitch_source].base_value = (-0.2 / 12.0) * 0.5;

	/////////////////////////
	seq.tracks[7].velocity_multiplier = 1.4;
	seq.tracks[8].velocity_multiplier = 1.4;
	seq.sources[seq.tracks[7].volume_source].base_value = -0.5;
	seq.sources[seq.tracks[8].volume_source].base_value = -0.5;
	seq.sources[seq.tracks[7].pan_source].base_value = -0.1;
	seq.sources[seq.tracks[8].pan_source].base_value = 0.1;

	////////////////////////
	seq.tracks[0].vibrato_source = 8;
	seq.sources[8].multiplier = 1.4;
	
	seq.tracks[0].instrument = 0;
	seq.tracks[1].instrument = 1;
	seq.tracks[2].instrument = 2;
	seq.tracks[3].instrument = 3;
	seq.tracks[4].instrument = 4;
	seq.tracks[5].instrument = 5;
	seq.tracks[6].instrument = 6;
	seq.tracks[7].instrument = 8;
	seq.tracks[8].instrument = 9;
	*/
	m64.clear();
	m64 = seq.create_m64();
	
	out_filename = filename.substr(0, filename.find_last_of("."));
	out_filename += ".m64";
	remove(out_filename.c_str());
	output.open(out_filename, ios::out | ios::binary);
	output.write((const char*)&m64[0], m64.size());
	output.close();
	

	return 0;
}