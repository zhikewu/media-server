// ITU-T H.222.0(06/2012)
// Information technology �C Generic coding of moving pictures and associated audio information: Systems
// 2.4.3.1 Transport stream(p34)

#include "mpeg-util.h"
#include "mpeg-ts-proto.h"
#include "mpeg-ts.h"
#include "cstringext.h"
#include "h264-util.h"
#include "crc32.h"
#include <memory.h>
#include <string.h>
#include <assert.h>

#define N_MPEG_TS_STREAM	8

typedef struct _mpeg_ts_enc_context_t
{
	pat_t pat;

	unsigned int pat_period;
	unsigned int pcr_period;

	mpeg_ts_cbwrite write;
	void* param;
} mpeg_ts_enc_context_t;

static void mpeg_ts_write_section_header(const mpeg_ts_enc_context_t *ts, int pid, int cc, const void* payload, size_t len)
{
	uint8_t data[TS_PACKET_SIZE];

	assert(len < TS_PACKET_SIZE - 5); // TS-header + pointer

	// TS Header

	// sync_byte
	data[0] = 0x47;
	// transport_error_indicator = 0
	// payload_unit_start_indicator = 1
	// transport_priority = 0
	data[1] = 0x40 | ((pid >> 8) & 0x1F);
	data[2] = pid & 0xFF;
	// transport_scrambling_control = 0x00
	// adaptation_field_control = 0x11 adaptation and payload
	data[3] = 0x10 | (cc & 0x0F);

//	// Adaptation
//	if(len < TS_PACKET_SIZE - 5)
//	{
//		data[3] |= 0x20; // with adaptation
//		data[4] = TS_PACKET_SIZE - len - 5 - 1; // 4B-Header + 1B-pointer + 1B-self
//		if(data[4] > 0)
//		{
//			// adaptation
//			data[5] = 0; // no flag
//			memset(data+6, 0xFF, data[4]-1);
//		}
//	}

	// pointer
	//data[TS_PACKET_SIZE-len-1] = 0x00;
    data[4] = 0x00;

    // TS Payload
    //memmove(data + TS_PACKET_SIZE - len, payload, len);
    memmove(data + 5, payload, len);
    memset(data+5+len, 0xff, TS_PACKET_SIZE-len-5);

	ts->write(ts->param, data, TS_PACKET_SIZE);
}

#define TS_AF_FLAG_PCR(flag) ((flag) & 0x10)

static int ts_write_pes(mpeg_ts_enc_context_t *tsctx, pes_t *stream, const uint8_t* payload, size_t bytes)
{
	// 2.4.3.6 PES packet
	// Table 2-21

	size_t len = 0;
	int start = 1; // first packet
//    int keyframe = 0; // video IDR-frame
	uint8_t *p = NULL;
	uint8_t *pes = NULL;
	uint8_t data[TS_PACKET_SIZE];
	int64_t pcr = 0x8000000000000000L;

	while(bytes > 0)
	{
		stream->cc = (stream->cc + 1 ) % 16;

		// TS Header
		data[0] = 0x47;	// sync_byte
		data[1] = 0x00 | ((stream->pid >>8) & 0x1F);
		data[2] = stream->pid & 0xFF;
		data[3] = 0x10 | (stream->cc & 0x0F); // no adaptation, payload only
		data[4] = 0; // clear adaptation length
		data[5] = 0; // clear adaptation flags

		// 2.7.2 Frequency of coding the program clock reference
		// http://www.bretl.com/mpeghtml/SCR.HTM
		// the maximum between PCRs is 100ms.  
		if(start && stream->pid == tsctx->pat.pmt[0].PCR_PID)
		{
			data[3] |= 0x20; // AF
			data[5] |= 0x10; // PCR_flag
		}

		//if(start && STREAM_VIDEO_H264==stream->avtype && h264_idr(payload, bytes))
		//{
		//	//In the PCR_PID the random_access_indicator may only be set to '1' 
		//	//in a transport stream packet containing the PCR fields.
		//	data[3] |= 0x20;
		//	data[5] |= 0x50; // random_access_indicator + PCR_flag
		//}

		if(data[3] & 0x20)
		{
			data[4] = 1; // 1-flag

			if(TS_AF_FLAG_PCR(data[5]))
			{
				data[4] += 6; // 6-PCR
				pcr = stream->pts * 300 - 100; // TODO: delay???
				pcr_write(data + 6, pcr);
			}

			pes = data + 4 + 1 + data[4]; // 4-TS + 1-AF-Len + AF-Payload
		}
		else
		{
			pes = data + 4;
		}

		p = pes;

		// PES header
		if(start)
		{
			data[1] |= 0x40; // payload_unit_start_indicator

			p = pes + pes_write_header(stream->pts, stream->dts, stream->sid, pes);

			if(PSI_STREAM_H264 == stream->avtype && 0x09 != h264_type(payload, bytes))
			{
				// 2.14 Carriage of Rec. ITU-T H.264 | ISO/IEC 14496-10 video
				// Each AVC access unit shall contain an access unit delimiter NAL Unit
				le_write_uint32(p, 0x00000001);
				p[4] = 0x09; // AUD
				p[5] = 0xE0; // any slice type (0xe) + rbsp stop one bit
				p += 6;
			}

			// PES_packet_length = PES-Header + Payload-Size
			// A value of 0 indicates that the PES packet length is neither specified nor bounded 
			// and is allowed only in PES packets whose payload consists of bytes from a 
			// video elementary stream contained in transport stream packets
			if((p - pes - 6) + bytes > 0xFFFF)
				le_write_uint16(pes + 4, 0); // 2.4.3.7 PES packet => PES_packet_length
			else
				le_write_uint16(pes + 4, (uint16_t)((p - pes - 6) + bytes));
		}

		len = p - data; // TS + PES header length
		if(len + bytes < TS_PACKET_SIZE)
		{
			// move pes header
			if(start)
			{
				memmove(data + (TS_PACKET_SIZE - bytes - (p - pes)), pes, p - pes);
			}

			// adaptation
			if(data[3] & 0x20) // has AF?
			{
				memset(data + 5 + data[4], 0xFF, TS_PACKET_SIZE - (len + bytes));
				data[4] += (uint8_t)(TS_PACKET_SIZE - (len + bytes));
			}
			else
			{
				memset(data + 4, 0xFF, TS_PACKET_SIZE - (len + bytes));
				data[3] |= 0x20;
				data[4] = (uint8_t)(TS_PACKET_SIZE - (len + bytes) - 1);
				data[5] = 0; // no flag				
			}

			len = bytes;
			p = data + 5 + data[4] + (p - pes);
		}
		else
		{
			len = TS_PACKET_SIZE - len;
		}

		// payload
		memcpy(p, payload, len);

		payload += len;
		bytes -= len;
		start = 0;

		// send with TS-header
		tsctx->write(tsctx->param, data, TS_PACKET_SIZE);
	}

	return 0;
}

int mpeg_ts_write(void* ts, int avtype, int64_t pts, int64_t dts, const void* data, size_t bytes)
{
	size_t i, r;
	pes_t *stream = NULL;
	mpeg_ts_enc_context_t *tsctx;
	uint8_t payload[TS_PACKET_SIZE];

	tsctx = (mpeg_ts_enc_context_t*)ts;

	if(0 == tsctx->pat_period)
	{
		// PAT
		tsctx->pat.cc = (tsctx->pat.cc + 1) % 16;
		r = pat_write(&tsctx->pat, payload);
		mpeg_ts_write_section_header(ts, 0x00, tsctx->pat.cc, payload, r); // PID = 0x00 program association table

		// PMT
		for(i = 0; i < tsctx->pat.pmt_count; i++)
		{
			tsctx->pat.pmt[i].cc = (tsctx->pat.pmt[i].cc + 1) % 16;
			r = pmt_write(&tsctx->pat.pmt[i], payload);
			mpeg_ts_write_section_header(ts, tsctx->pat.pmt[i].pid, tsctx->pat.pmt[i].cc, payload, r);
		}
	}

	tsctx->pat_period = (tsctx->pat_period + 1) % 200;

	// Elementary Stream
	for(i = 0; i < tsctx->pat.pmt[0].stream_count; i++)
	{
		stream = &tsctx->pat.pmt[0].streams[i];
		if(avtype == (int)stream->avtype)
		{
			stream->pts = pts;
			stream->dts = dts;
			break;
		}
	}

	ts_write_pes(tsctx, stream, data, bytes);
	return 0;
}

void* mpeg_ts_create(mpeg_ts_cbwrite func, void* param)
{
	mpeg_ts_enc_context_t *tsctx = NULL;

	assert(func);
	tsctx = (mpeg_ts_enc_context_t *)malloc(sizeof(mpeg_ts_enc_context_t) 
											+ sizeof(tsctx->pat.pmt[0])
											+ 2 * sizeof(tsctx->pat.pmt[0].streams[0]));
	if(!tsctx)
		return NULL;

	memset(tsctx, 0, sizeof(mpeg_ts_enc_context_t));
//	mpeg_ts_reset(tsctx);
    tsctx->pat_period = 0;
    tsctx->pcr_period = 0;

    tsctx->pat.tsid = 1;
    tsctx->pat.ver = 0;
    tsctx->pat.cc = (uint32_t)-1; // +1 => 0

    tsctx->pat.pmt_count = 1; // only one program in ts
    tsctx->pat.pmt = (pmt_t*)(tsctx + 1);
    tsctx->pat.pmt[0].pid = 0x100;
    tsctx->pat.pmt[0].pn = 1;
    tsctx->pat.pmt[0].ver = 0x00;
    tsctx->pat.pmt[0].cc = (uint32_t)-1; // +1 => 0
    tsctx->pat.pmt[0].pminfo_len = 0;
    tsctx->pat.pmt[0].pminfo = NULL;
    tsctx->pat.pmt[0].PCR_PID = 0x101; // 0x1FFF-don't set PCR

    tsctx->pat.pmt[0].stream_count = 2; // H.264 + AAC
    tsctx->pat.pmt[0].streams = (pes_t*)(tsctx->pat.pmt + 1);
    tsctx->pat.pmt[0].streams[0].pmt = &tsctx->pat.pmt[0];
    tsctx->pat.pmt[0].streams[0].pid = 0x101;
    tsctx->pat.pmt[0].streams[0].sid = PES_SID_VIDEO;
	tsctx->pat.pmt[0].streams[0].avtype = PSI_STREAM_H264;
    tsctx->pat.pmt[0].streams[0].esinfo_len = 0x00;
    tsctx->pat.pmt[0].streams[0].esinfo = NULL;
    tsctx->pat.pmt[0].streams[0].cc = (uint8_t)(-1); // +1 => 0
    tsctx->pat.pmt[0].streams[1].pmt = &tsctx->pat.pmt[0];
    tsctx->pat.pmt[0].streams[1].pid = 0x102;
    tsctx->pat.pmt[0].streams[1].sid = PES_SID_AUDIO;
	tsctx->pat.pmt[0].streams[1].avtype = PSI_STREAM_AAC;
    tsctx->pat.pmt[0].streams[1].esinfo_len = 0x00;
    tsctx->pat.pmt[0].streams[1].esinfo = NULL;
    tsctx->pat.pmt[0].streams[1].cc = (uint8_t)(-1); // +1 => 0

	tsctx->write = func;
	tsctx->param = param;
	return tsctx;
}

int mpeg_ts_destroy(void* ts)
{
	uint32_t i, j;
	mpeg_ts_enc_context_t *tsctx = NULL;
	tsctx = (mpeg_ts_enc_context_t*)ts;

	for(i = 0; i < tsctx->pat.pmt_count; i++)
	{
		for(j = 0; j < tsctx->pat.pmt[i].stream_count; j++)
		{
			if(tsctx->pat.pmt[i].streams[j].esinfo)
				free(tsctx->pat.pmt[i].streams[j].esinfo);
		}
	}

	free(tsctx);
	return 0;
}

int mpeg_ts_reset(void* ts)
{
	mpeg_ts_enc_context_t *tsctx;
	tsctx = (mpeg_ts_enc_context_t*)ts;
	tsctx->pat_period = 0;
	tsctx->pcr_period = 0;
	return 0;
}

int mpeg_ts_add_stream(void* ts, int avtype)
{
	pmt_t *pmt = NULL;
	mpeg_ts_enc_context_t *tsctx;

	tsctx = (mpeg_ts_enc_context_t*)ts;
	if(pmt->stream_count + 1 >= N_MPEG_TS_STREAM)
	{
		assert(0);
		return -1;
	}

	pmt = &tsctx->pat.pmt[0];
	pmt->streams[pmt->stream_count].avtype = (uint8_t)avtype;
	pmt->streams[pmt->stream_count].pid = (uint16_t)(TS_PID_USER + pmt->stream_count);
	pmt->streams[pmt->stream_count].esinfo_len = 0;
	pmt->streams[pmt->stream_count].esinfo = NULL;

	// stream id
	// Table 2-22 �C Stream_id assignments
	if(PSI_STREAM_H264==avtype || PSI_STREAM_MPEG4==avtype || PSI_STREAM_MPEG2==avtype || PSI_STREAM_MPEG1==avtype || PSI_STREAM_VIDEO_VC1==avtype || PSI_STREAM_VIDEO_SVAC==avtype)
	{
		// Rec. ITU-T H.262 | ISO/IEC 13818-2, ISO/IEC 11172-2, ISO/IEC 14496-2 
		// or Rec. ITU-T H.264 | ISO/IEC 14496-10 video stream number
		pmt->streams[pmt->stream_count].sid = PES_SID_VIDEO;
	}
	else if(PSI_STREAM_AAC==avtype || PSI_STREAM_MPEG4_AAC_LATM==avtype || PSI_STREAM_MPEG4_AAC==avtype || PSI_STREAM_MP3==avtype || PSI_STREAM_AUDIO_AC3==avtype || PSI_STREAM_AUDIO_SVAC==avtype)
	{
		// ISO/IEC 13818-3 or ISO/IEC 11172-3 or ISO/IEC 13818-7 or ISO/IEC 14496-3
		// audio stream number
		pmt->streams[pmt->stream_count].sid = PES_SID_AUDIO;
	}
	else
	{
		// private_stream_1
		pmt->streams[pmt->stream_count].sid = PES_SID_PRIVATE_1;
	}

	++pmt->stream_count;
	pmt->ver = (pmt->ver+1) % 32;

	tsctx->pat_period = 0; // immediate update pat/pmt
	return 0;
}
