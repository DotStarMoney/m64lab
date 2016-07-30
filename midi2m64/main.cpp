#include "midi/inc/MidiFile.h"
#include "midi/inc/Options.h"
#include <iostream>
#include <vector>
#include <string>
#include <fstream>
#include <stdio.h>
#include <math.h>
#include <limits>
#include <stdexcept>
using namespace std;

// TODO:
//     + Fix optimize events: remove-coincidental events should be based on "is next in series of rests" not "next event is a rest" 
//     + Debug pitch bend refactoring
//     + Determine m64 volume scaling, definitely sounds wrong
//     + Build midi parsing into seq class
//     + Add UI  

#ifndef _NDEBUG
#define DEBUG_MIDI_FILE "LastImpactElectro.mid"
#endif

#define NOTE_BIAS 21

#define NOTE_GROUP_MAX_GAP 288
#define MIN_AVG_GAP 2.0

#define PERC_BANK_INSTRUMENT_N 0x7f

unsigned short bit_mask_from_value[16] =
	{	0x0001, 0x0003, 0x0007, 0x000F,
		0x001F, 0x003F, 0x007F, 0x00FF,
		0x01FF, 0x03FF, 0x07FF, 0x0FFF,
		0x1FFF, 0x3FFF, 0x7FFF, 0xFFFF};

short rev_short(short _x)
{
	return ((unsigned short)_x >> 8) | (_x << 8);
}


class NoteRemapping
{
public:
	NoteRemapping()
	{
		int i;
		for (i = 0; i < 256; i++)
		{
			mapping[i] = i;
		}
	}
	int& operator[](unsigned char _index)
	{
		return mapping[_index];
	}
	const int& operator[](unsigned char _index) const
	{
		return mapping[_index];
	}
private:
	int mapping[256];
};

// Technically impressive, but needs to be built in to event optimization,
//    can't use alone
void opt_avg_intervals(
	float* _data,
	size_t _data_n,
	size_t _desired_n,
	size_t* _res
)
{
	float** L;
	double** best;
	double global_best;
	double this_best;
	size_t* start;
	size_t* start_best;
	double* total_err;
	double avg;
	double err_sq;
	double this_err;
	double total;
	int i;
	int q;
	int k;
	int start_base;
	bool proceed_flag;
	bool escape_flag;

	_desired_n++;

	L = (float**)malloc(sizeof(float*)*(_data_n - 1));
	for (i = 0; i < _data_n; i++)
	{
		L[i] = (float*)malloc(sizeof(float)*(_data_n - i));
		for (q = 0; q < _data_n; q++)
		{
			total = 0;
			err_sq = 0;
			for (k = i; k <= q; k++)
			{
				total += _data[k];
			}
			avg = total / (q - i + 1.0);
			for (k = i; k <= q; k++)
			{
				this_err = _data[k] - avg;
				err_sq = this_err*this_err;
			}
			L[i][q - i] = err_sq;
		}
	}
	
	best = (double**)malloc(sizeof(double*)*(_desired_n - 3));
	for (i = 0; i < _desired_n - 3; i++)
	{
		best[i] = (double*)malloc(sizeof(double)*_data_n);
		for (q = 0; q < _data_n; q++)
		{
			best[i][q] = -1.0;
		}
	}
	global_best = FLT_MAX;
	start_best = _res;
	for (i = 0; i < _desired_n - 1; i++)
	{
		start_best[i] = i;
	}
	start = (size_t*)malloc(sizeof(size_t)*(_desired_n - 1));
	total_err = (double*)malloc(sizeof(double)*(_desired_n - 1));

	for (q = 0; q <= (_data_n - _desired_n); q++)
	{
		start[0] = q;
		total_err[0] = L[0][start[0]];
		start[1] = start[0] + 1;
		i = 1;
		do
		{
			proceed_flag = false;
			if (i < _desired_n - 2)
			{
				start_base = start[i - 1] + 1;
				total_err[i] = total_err[i - 1] +
					L[start_base][start[i] - start_base];
				this_best = best[i - 1][start[i] + 1];
				if ((this_best = -1.0) && (total_err[i] < global_best))
				{
					i += 1;
					if (i == (_desired_n - 2))
					{
						start[i] = _data_n - 1;
					}
					else
					{
						start[i] = start[i - 1] + 1;
					}
				}
				else
				{
					if ((this_best != -1.0) && ((this_best +
						total_err[i]) < global_best))
					{
						global_best = this_best + total_err[i];
						for (k = 0; k < _desired_n - 1; k++)
						{
							start_best[k] = start[k];
						}
					}
					proceed_flag = true;
				}
			}
			else
			{
				start_base = start[i - 1] + 1;
				total_err[i] = total_err[i - 1] +
					L[start_base][start[i] - start_base];
				if (total_err[i] < global_best)
				{
					global_best = total_err[i];
					for (k = 0; k < _desired_n - 1; k++)
					{
						start_best[k] = start[k];
					}
				}
				proceed_flag = true;
			}
			escape_flag = false;
			if (proceed_flag)
			{
				do
				{
					start[i]++;
					if (start[i] > (_data_n - _desired_n + i + 1))
					{
						if (i == 1)
						{
							escape_flag = true;
						}
						else
						{
							i--;
							best[i - 1][start[i] + 1] = global_best - 
								total_err[i];
						}
					}
					else
					{
						break;
					}
				} while (!escape_flag);
			}
		} while (!escape_flag);
	}

	free(total_err);
	free(start);
	for (i = 0; i < _desired_n - 3; free(best[i++]));
	free(best);
	for (i = 0; i < _data_n; free(L[i++]));
	free(L);
}

enum class ControllerSourceType
{
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
		owner_track_name = "";
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
	string owner_track_name;
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
		fine_pitch_source = PARAM_SOURCE_NONE;
		volume_source = PARAM_SOURCE_NONE;
		pan_source = PARAM_SOURCE_NONE;
		echo_source = PARAM_SOURCE_NONE;
		vibrato_source = PARAM_SOURCE_NONE;
		instrument = 0;
		velocity_multiplier = 1.0;
		map_directly = false;
	}
	void remap(NoteRemapping& _mapping)
	{
		int i;
		for (i = 0; i < notes.size(); i++)
		{
			notes[i].note = (unsigned char)_mapping[notes[i].note];
		}
		map_directly = true;
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
	int fine_pitch_source;
	int volume_source;
	int pan_source;
	int echo_source;
	int vibrato_source;
	float velocity_multiplier;
	bool map_directly;
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
	void optimize(Track& _track, ControllerSource& _source)
	{
		int cur_event;
		int this_note;
		int this_ticks;
		int next_ticks;
		int last_rest_ticks;
		float last_value;
		NoteType last_type;

		
		this_note = 0;
		last_type = NoteType::Note;
		cur_event = 0; 
		
		while(cur_event < _source.events.size())
		{
			while (_track.notes[this_note].ticks <=
				_source.events[cur_event].ticks)
			{
				this_note++;
				if (this_note >= _track.notes.size()) break;
			}
			this_note--;
			this_ticks = _track.notes[this_note].ticks;
			if ((_track.notes[this_note].type == NoteType::Rest) ||
				((this_ticks == _source.events[cur_event].ticks) && 
					(_track.notes[this_note].type == NoteType::Note)))
			{
				if (last_type == NoteType::Rest) 
				{
					_source.events.erase(_source.events.begin() + 
						cur_event - 1);
				}
				else
				{
					cur_event++;
				}
			}
			else
			{
				cur_event++;
			}
			last_type = _track.notes[this_note].type;
		}
		cur_event--;
		if (_track.notes[_track.notes.size() - 1].type != NoteType::Note)
		{
			last_rest_ticks = _track.notes[_track.notes.size() - 1].ticks;
			if (_source.events[cur_event].ticks >= last_rest_ticks)
			{
				_source.events.pop_back();
			}
		}
		

		last_value = _source.events[0].value;
		cur_event = 1;
		while (cur_event < _source.events.size())
		{
			if (_source.events[cur_event].value == last_value)
			{
				_source.events.erase(_source.events.begin() + cur_event);
			}
			else
			{
				last_value = _source.events[cur_event].value;
				cur_event++;
			}
		}
	}
	void optimize_track_sources(int _track_number)
	{
		if (tracks[_track_number].echo_source != PARAM_SOURCE_NONE)
		{
			optimize(tracks[_track_number], 
				sources[tracks[_track_number].echo_source]);
		}
		if (tracks[_track_number].fine_pitch_source != PARAM_SOURCE_NONE)
		{
			optimize(tracks[_track_number], 
				sources[tracks[_track_number].fine_pitch_source]);
		}
		if (tracks[_track_number].pan_source != PARAM_SOURCE_NONE)
		{
			optimize(tracks[_track_number], 
				sources[tracks[_track_number].pan_source]);
		}
		if (tracks[_track_number].vibrato_source != PARAM_SOURCE_NONE)
		{
			optimize(tracks[_track_number], 
				sources[tracks[_track_number].vibrato_source]);
		}
		if (tracks[_track_number].volume_source != PARAM_SOURCE_NONE)
		{
			optimize(tracks[_track_number], 
				sources[tracks[_track_number].volume_source]);
		}
	}
	void optimize_all()
	{
		int i;
		for (i = 0; i < tracks.size(); i++)
		{
			optimize_track_sources(i);
		}
	}
	void refactor_notes_to_pitch_bend(Track& _track, ControllerSource& _source)
	{
		int i;
		int j;
		int k;
		int start_j;
		int ticks;
		int next_note_ticks;
		float semitone_shift;
		float semitone_offset;
		float value_adjust;
		bool has_events_flag;
		j = 0;
		for (i = 0; i < _track.notes.size(); i++)
		{
			k = 1;
			if (_track.notes[i].type == NoteType::Note)
			{
				ticks = _track.notes[i].ticks;
				if (i == (_track.notes.size() - 1))
				{
					next_note_ticks = total_ticks;
				}
				else
				{
					next_note_ticks = _track.notes[i + 1].ticks;
				}
				if (_source.events[j].ticks < ticks)
				{
					has_events_flag = false;
					for (; j < _source.events.size(); j++)
					{
						if (_source.events[j].ticks > ticks)
						{
							j--;
							has_events_flag = true;
							break;
						}
					}
					if (!has_events_flag) j--;
					has_events_flag = true;
				}
				else
				{
					if (_source.events[j].ticks < next_note_ticks)
					{
						has_events_flag = true;
					}
					else
					{
						has_events_flag = false;
					}
				}
	
				if (has_events_flag)
				{
					do
					{
						semitone_shift = (_source.events[j].value*2.0 - 1.0)*
							source_fine_pitch_range;
						if (fabs(semitone_shift) > 12.0)
						{
							semitone_offset = 
								floor((ceil(fabs(semitone_shift) - 1.0) /
									12.0) + 0.5) * 12;
							semitone_offset *= signbit(semitone_shift) ? 
								-1.0 : 1.0;
							if (_source.events[j].ticks <= ticks)
							{
								_track.notes[i].note = 
									((int)_track.notes[i + k - 1].note) + 
										(int) semitone_offset;
							}
							else
							{
								_track.notes.insert(
									_track.notes.begin() + i + k,
									NoteEvent(
										NoteType::Note,
										_source.events[j].ticks,
										_track.notes[i].velocity,
										_track.notes[i + k - 1].note + 
											(int) semitone_offset));
								k++;
							}
							if (_source.events[j].ticks < ticks)
							{
								_source.events.insert(
									_source.events.begin() + j + 1,
									ControllerEvent(
										ticks,
										_source.events[j].value));
								j++;
							}
							start_j = j;
							value_adjust = (semitone_offset / 
								source_fine_pitch_range)*0.5;
							while (j < _source.events.size())
							{
								if (_source.events[j].ticks < next_note_ticks)
								{
									break;
								}
								_source.events[j].value -= value_adjust;
								j++;
							}
							j = start_j;
						}
						j++;
						if (j >= _source.events.size()) break;
					} while (_source.events[j].ticks < next_note_ticks);
					j--;
				}
			}
		}
	}
	void refactor_all_pitch_bends()
	{
		int i;
		for (i = 0; i < tracks.size(); i++)
		{
			if (tracks[i].fine_pitch_source != PARAM_SOURCE_NONE)
			{
				refactor_notes_to_pitch_bend(
					tracks[i],
					sources[tracks[i].fine_pitch_source]
				);
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
		int event_prev_values[256];

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
			for (j = 0; j < 256; event_prev_values[j++] = -1);
			last_tick = 0;
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

				if (event_prev_values[events[near_event].event_code] != 
					val_int) {
					if (tick != last_tick)
					{
						ADD(0xFD);
						ADD_V(tick - last_tick);
					}
					ADD(events[near_event].event_code);
					ADD(val_int);
					last_tick = tick;
				}

				event_prev_values[events[near_event].event_code] = val_int;
				events[near_event].cur_event++;
				if (events[near_event].event_source->events.size() ==
					events[near_event].cur_event)
				{
					events.erase(events.begin() + near_event);
				}
			} 
			if (last_tick != total_ticks)
			{
				ADD(0xFD);
				ADD_V(total_ticks - last_tick);
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
					if (!tracks[i].map_directly)
					{
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

					if (tracks[i].map_directly)
					{
						note_fmt = note;
					}
					else
					{
						note_fmt = note - (cur_note_group * 64 + NOTE_BIAS);
					}
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
	Track& get_track_by_name(const string& _name)
	{
		int i;
		for (i = 0; i < sources.size(); i++)
		{
			if (tracks[i].name.compare(_name) == 0)
			{
				return tracks[i];
			}
		}
		throw std::invalid_argument("No track of name \"" + _name + "\" exists.");
	}
	int new_fixed_source(float _value)
	{
		ControllerSource new_source;
		new_source.type = ControllerSourceType::UserFixed;
		new_source.events.push_back(ControllerEvent(0, _value));
		sources.push_back(new_source);
		return sources.size() - 1;
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
		for (i = 0; i < cur_sources.size(); i++)
		{
			cur_sources[i].owner_track_name = new_track.name;
		}
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
		seq.sources[i].owner_track_id = -1;
	}
	for (i = 0; i < seq.tracks.size(); i++)
	{
		if (seq.tracks[i].fine_pitch_source != PARAM_SOURCE_NONE)
		{
			seq.sources[seq.tracks[i].fine_pitch_source].owner_track_id = i;
		}
		if (seq.tracks[i].pan_source != PARAM_SOURCE_NONE)
		{
			seq.sources[seq.tracks[i].pan_source].owner_track_id = i;
		}
		if (seq.tracks[i].volume_source != PARAM_SOURCE_NONE)
		{
			seq.sources[seq.tracks[i].volume_source].owner_track_id = i;
		}
	}
	seq.convert_clock_base();
	seq.trim_events();


	///////////////////////////////////////////////////////////////////////////////////
	seq.get_track_by_name("Pad 1").fine_pitch_source = 
		seq.get_track_by_name("CrunchyLoop").fine_pitch_source;
	seq.get_track_by_name("CrunchyLoop").fine_pitch_source = PARAM_SOURCE_NONE;

	seq.get_track_by_name("Pad 2").fine_pitch_source =
		seq.get_track_by_name("EStreamLoop").fine_pitch_source;
	seq.get_track_by_name("EStreamLoop").fine_pitch_source = PARAM_SOURCE_NONE;
	seq.get_track_by_name("Pad 2").pan_source = seq.new_fixed_source(1.0 - 0.17);

	seq.get_track_by_name("CheddarCheese L").fine_pitch_source =
		seq.get_track_by_name("HitEffects").fine_pitch_source;
	seq.get_track_by_name("CheddarCheese R").fine_pitch_source =
		seq.get_track_by_name("HitEffects").fine_pitch_source;
	seq.get_track_by_name("HitEffects").fine_pitch_source = PARAM_SOURCE_NONE;

	seq.get_track_by_name("DistBell").echo_source = seq.new_fixed_source(1.0);
	seq.get_track_by_name("DistBell Echo").echo_source = seq.new_fixed_source(1.0);
	seq.get_track_by_name("Arpegginator").echo_source = seq.new_fixed_source(1.0);

	//seq.get_track_by_name("Battery").pan_source = PARAM_SOURCE_NONE;
	seq.get_track_by_name("Battery").instrument = PERC_BANK_INSTRUMENT_N;
	
	seq.bank = 0x25;

	NoteRemapping drums;
	drums[0x24] = 0x00;
	drums[0x25] = 0x01;
	drums[0x26] = 0x02;
	drums[0x2c] = 0x03;
	drums[0x32] = 0x04;
	seq.get_track_by_name("Battery").remap(drums);

	
	i = 0;
	while(i < seq.tracks.size())
	{
		if ((seq.tracks[i].name != "Pad 1"))// && (seq.tracks[i].name != "Pad 2") && (seq.tracks[i].name != "Battery"))
		{
			seq.tracks.erase(seq.tracks.begin() + i);
		}
		else
		{
			i++;
		}
	}
	
	seq.get_track_by_name("Pad 1").volume_source = PARAM_SOURCE_NONE;
	seq.get_track_by_name("Pad 1").fine_pitch_source = PARAM_SOURCE_NONE;


	seq.get_track_by_name("Pad 1").instrument = 8;
	//seq.get_track_by_name("Pad 2").instrument = 9;


	//seq.source_fine_pitch_range = 48;
	//seq.refactor_all_pitch_bends();
	
	seq.optimize_all();
	///////////////////////////////////////////////////////////////////////////////////
	
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