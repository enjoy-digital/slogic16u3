/***********************************************************************************************************************
*                                                                                                                      *
* libscopehal                                                                                                          *
*                                                                                                                      *
* Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>                                                                  *
* All rights reserved.                                                                                                 *
*                                                                                                                      *
* Redistribution and use in source and binary forms, with or without modification, are permitted provided that the    *
* following conditions are met:                                                                                        *
*                                                                                                                      *
*    * Redistributions of source code must retain the above copyright notice, this list of conditions, and the         *
*      following disclaimer.                                                                                           *
*                                                                                                                      *
*    * Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the       *
*      following disclaimer in the documentation and/or other materials provided with the distribution.                *
*                                                                                                                      *
*    * Neither the name of the author nor the names of any contributors may be used to endorse or promote products     *
*      derived from this software without specific prior written permission.                                          *
*                                                                                                                      *
* THIS SOFTWARE IS PROVIDED BY THE AUTHORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED   *
* TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL *
* THE AUTHORS BE HELD LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES        *
* (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR       *
* BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT *
* (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE       *
* POSSIBILITY OF SUCH DAMAGE.                                                                                          *
*                                                                                                                      *
***********************************************************************************************************************/

/**
	@file
	@brief Implementation of SLogicOscilloscope
	@ingroup scopedrivers

	Data plane (waveform socket): 'K' request, then u32 seqnum, u16 nchannels, i64 fs_per_sample, i64 trigger_fs,
	f64 wfms_s, and per channel: u64 channel, u64 bytes, i32 first sample offset, bytes of bits (sample 8*m+b on
	bit b of byte m).
 */

#include "scopehal.h"
#include "SLogicOscilloscope.h"
#include "EdgeTrigger.h"

using namespace std;

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//Construction / destruction

/**
	@brief Initialize the driver

	@param transport	SCPITwinLanTransport pointing at a slogic_scopehal_bridge instance
 */
SLogicOscilloscope::SLogicOscilloscope(SCPITransport* transport)
	: SCPIDevice(transport)
	, SCPIInstrument(transport)
	, RemoteBridgeOscilloscope(transport, true)
	, m_digitalChannelCount(16)
{
	//16 digital channels, hardware names "0".."15" (bridge channel numbers)
	static const char* colors[8] = { "#ffff00", "#ff6abc", "#00ffff", "#00c100", "#d7ffd7", "#8482ff", "#ff0000", "#ff8000" };
	for(size_t i = 0; i < m_digitalChannelCount; i++)
	{
		auto chan = new OscilloscopeChannel(
			this,
			to_string(i),
			colors[i % 8],
			Unit(Unit::UNIT_FS),
			Unit(Unit::UNIT_COUNTS),
			Stream::STREAM_TYPE_DIGITAL,
			i);
		m_channels.push_back(chan);
		chan->SetDisplayName("CH" + to_string(i));
	}

	auto csock = dynamic_cast<SCPITwinLanTransport*>(m_transport);
	if(!csock)
		LogFatal("SLogicOscilloscope expects a SCPITwinLanTransport\n");

	SetSampleRate(100000000L);
	SetSampleDepth(100000);

	//Edge trigger on CH0
	auto trig = new EdgeTrigger(this);
	trig->SetType(EdgeTrigger::EDGE_RISING);
	trig->SetLevel(0.5);
	trig->SetInput(0, StreamDescriptor(GetOscilloscopeChannel(0)));
	SetTrigger(trig);
	PushTrigger();
	SetTriggerOffset(10000000000LL); //10us
}

SLogicOscilloscope::~SLogicOscilloscope()
{
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//Accessors

string SLogicOscilloscope::GetDriverNameInternal()
{
	return "slogic";
}

unsigned int SLogicOscilloscope::GetInstrumentTypes() const
{
	return Instrument::INST_OSCILLOSCOPE;
}

uint32_t SLogicOscilloscope::GetInstrumentTypesForChannel(size_t /*i*/) const
{
	return Instrument::INST_OSCILLOSCOPE;
}

void SLogicOscilloscope::FlushConfigCache()
{
	lock_guard<recursive_mutex> lock(m_cacheMutex);
}

vector<OscilloscopeChannel::CouplingType> SLogicOscilloscope::GetAvailableCouplings(size_t /*i*/)
{
	return vector<OscilloscopeChannel::CouplingType>();
}

double SLogicOscilloscope::GetChannelAttenuation(size_t /*i*/)
{
	return 1;
}

void SLogicOscilloscope::SetChannelAttenuation(size_t /*i*/, double /*atten*/)
{
}

unsigned int SLogicOscilloscope::GetChannelBandwidthLimit(size_t /*i*/)
{
	return 0;
}

void SLogicOscilloscope::SetChannelBandwidthLimit(size_t /*i*/, unsigned int /*limit_mhz*/)
{
}

OscilloscopeChannel* SLogicOscilloscope::GetExternalTrigger()
{
	return nullptr;
}

bool SLogicOscilloscope::CanEnableChannel(size_t /*i*/)
{
	return true;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//Acquisition

Oscilloscope::TriggerMode SLogicOscilloscope::PollTrigger()
{
	//Waveforms are blocking on the data socket: always report "triggered" (AcquireData waits)
	return TRIGGER_MODE_TRIGGERED;
}

bool SLogicOscilloscope::AcquireData()
{
	const uint8_t req = 'K';
	m_transport->SendRawData(1, &req);

	uint32_t seqnum;
	uint16_t numChannels;
	int64_t fs_per_sample;
	int64_t trigger_fs;
	double wfms_s;
	if(!m_transport->ReadRawData(sizeof(seqnum), reinterpret_cast<uint8_t*>(&seqnum)) ||
		!m_transport->ReadRawData(sizeof(numChannels), reinterpret_cast<uint8_t*>(&numChannels)) ||
		!m_transport->ReadRawData(sizeof(fs_per_sample), reinterpret_cast<uint8_t*>(&fs_per_sample)) ||
		!m_transport->ReadRawData(sizeof(trigger_fs), reinterpret_cast<uint8_t*>(&trigger_fs)) ||
		!m_transport->ReadRawData(sizeof(wfms_s), reinterpret_cast<uint8_t*>(&wfms_s)))
		return false;

	SequenceSet s;
	double t = GetTime();
	int64_t fs = (t - floor(t)) * FS_PER_SECOND;

	for(size_t i = 0; i < numChannels; i++)
	{
		uint64_t chnum;
		uint64_t nbytes;
		int32_t first_sample;
		if(!m_transport->ReadRawData(sizeof(chnum), reinterpret_cast<uint8_t*>(&chnum)) ||
			!m_transport->ReadRawData(sizeof(nbytes), reinterpret_cast<uint8_t*>(&nbytes)) ||
			!m_transport->ReadRawData(sizeof(first_sample), reinterpret_cast<uint8_t*>(&first_sample)))
			return false;

		vector<uint8_t> buf(nbytes);
		if(nbytes && !m_transport->ReadRawData(nbytes, buf.data()))
			return false;
		if(chnum >= m_digitalChannelCount || nbytes == 0)
			continue;

		//Bits to a sparse (deduplicated) digital waveform
		auto cap = new SparseDigitalWaveform;
		cap->m_timescale = fs_per_sample;
		cap->m_triggerPhase = 0;
		cap->m_startTimestamp = time(NULL);
		cap->m_startFemtoseconds = fs;
		cap->PrepareForCpuAccess();
		cap->Resize(nbytes * 8);

		size_t k = 0;
		bool last = buf[0] & 1;
		cap->m_offsets[0] = first_sample;
		cap->m_durations[0] = 1;
		cap->m_samples[0] = last;
		size_t nsamples = nbytes * 8;
		for(size_t m = 1; m < nsamples; m++)
		{
			bool sample = (buf[m / 8] >> (m % 8)) & 1;
			if(sample == last)
				cap->m_durations[k]++;
			else
			{
				k++;
				cap->m_offsets[k] = first_sample + m;
				cap->m_durations[k] = 1;
				cap->m_samples[k] = sample;
				last = sample;
			}
		}
		cap->Resize(k + 1);
		cap->MarkSamplesModifiedFromCpu();
		cap->MarkTimestampsModifiedFromCpu();
		s[GetOscilloscopeChannel(chnum)] = cap;
	}

	{
		lock_guard<recursive_mutex> lock(m_mutex);
		m_triggerOffset = trigger_fs;
	}

	//Save the waveforms to our queue (keep the latest ones only)
	m_pendingWaveformsMutex.lock();
	m_pendingWaveforms.push_back(s);
	while(m_pendingWaveforms.size() > 2)
	{
		for(auto it : *m_pendingWaveforms.begin())
			delete it.second;
		m_pendingWaveforms.pop_front();
	}
	m_pendingWaveformsMutex.unlock();

	if(m_triggerOneShot)
		m_triggerArmed = false;
	return true;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//Timebase

vector<uint64_t> SLogicOscilloscope::ParseList(const string& query, bool interval)
{
	vector<uint64_t> ret;
	string reply;
	{
		lock_guard<recursive_mutex> lock(m_mutex);
		m_transport->SendCommand(query);
		reply = m_transport->ReadReply();
	}
	size_t start = 0;
	while(start < reply.size())
	{
		size_t end = reply.find(',', start);
		if(end == string::npos)
			end = reply.size();
		auto item = reply.substr(start, end - start);
		if(!item.empty())
		{
			double v = stod(item);
			ret.push_back(interval ? (uint64_t)(FS_PER_SECOND / v + 0.5) : (uint64_t)v);
		}
		start = end + 1;
	}
	return ret;
}

vector<uint64_t> SLogicOscilloscope::GetSampleRatesNonInterleaved()
{
	return ParseList("RATES?", true);
}

vector<uint64_t> SLogicOscilloscope::GetSampleRatesInterleaved()
{
	return vector<uint64_t>();
}

set<Oscilloscope::InterleaveConflict> SLogicOscilloscope::GetInterleaveConflicts()
{
	return set<Oscilloscope::InterleaveConflict>();
}

vector<uint64_t> SLogicOscilloscope::GetSampleDepthsNonInterleaved()
{
	return ParseList("DEPTHS?", false);
}

vector<uint64_t> SLogicOscilloscope::GetSampleDepthsInterleaved()
{
	return vector<uint64_t>();
}

bool SLogicOscilloscope::IsInterleaving()
{
	return false;
}

bool SLogicOscilloscope::SetInterleaving(bool /*combine*/)
{
	return false;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//ADC configuration (no analog channels)

vector<Oscilloscope::AnalogBank> SLogicOscilloscope::GetAnalogBanks()
{
	return vector<AnalogBank>();
}

Oscilloscope::AnalogBank SLogicOscilloscope::GetAnalogBank(size_t /*channel*/)
{
	return AnalogBank();
}

bool SLogicOscilloscope::IsADCModeConfigurable()
{
	return false;
}

vector<string> SLogicOscilloscope::GetADCModeNames(size_t /*channel*/)
{
	return vector<string>();
}

size_t SLogicOscilloscope::GetADCMode(size_t /*channel*/)
{
	return 0;
}

void SLogicOscilloscope::SetADCMode(size_t /*channel*/, size_t /*mode*/)
{
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//Logic analyzer configuration

vector<Oscilloscope::DigitalBank> SLogicOscilloscope::GetDigitalBanks()
{
	vector<DigitalBank> banks;
	DigitalBank bank;
	for(size_t i = 0; i < m_digitalChannelCount; i++)
		bank.push_back(GetOscilloscopeChannel(i));
	banks.push_back(bank);
	return banks;
}

Oscilloscope::DigitalBank SLogicOscilloscope::GetDigitalBank(size_t /*channel*/)
{
	return GetDigitalBanks()[0];
}

bool SLogicOscilloscope::IsDigitalHysteresisConfigurable()
{
	return false;
}

bool SLogicOscilloscope::IsDigitalThresholdConfigurable()
{
	return false; //Fixed input threshold (gateware), DAC threshold to come
}

float SLogicOscilloscope::GetDigitalHysteresis(size_t /*channel*/)
{
	return 0;
}

float SLogicOscilloscope::GetDigitalThreshold(size_t /*channel*/)
{
	return 1.6;
}

void SLogicOscilloscope::SetDigitalHysteresis(size_t /*channel*/, float /*level*/)
{
}

void SLogicOscilloscope::SetDigitalThreshold(size_t /*channel*/, float /*level*/)
{
}
