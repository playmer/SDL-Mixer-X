/*
  SDL_mixer:  An audio mixer library based on the SDL library
  Copyright (C) 1997-2017 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.
*/

#ifdef MUSIC_WAV

/* This file supports streaming WAV files */

#include "music_wav.h"
#include "music_id3tag.h"

typedef struct {
    SDL_bool active;
    Uint32 start;
    Uint32 stop;
    Uint32 initial_play_count;
    Uint32 current_play_count;
} WAVLoopPoint;

typedef struct {
    SDL_RWops *src;
    SDL_bool freesrc;
    SDL_AudioSpec spec;
    int volume;
    int play_count;
    Sint64 start;
    Sint64 stop;
    Sint64 samplesize;
    Uint8 *buffer;
    SDL_AudioStream *stream;
    int numloops;
    WAVLoopPoint *loops;
    Mix_MusicMetaTags tags;
    Uint16 encoding;
    int (*decode)(void *music, int length);
} WAV_Music;

/*
    Taken with permission from SDL_wave.h, part of the SDL library,
    available at: http://www.libsdl.org/
    and placed under the same license as this mixer library.
*/

/* WAVE files are little-endian */

/*******************************************/
/* Define values for Microsoft WAVE format */
/*******************************************/
#define RIFF        0x46464952      /* "RIFF" */
#define WAVE        0x45564157      /* "WAVE" */
#define FMT         0x20746D66      /* "fmt " */
#define DATA        0x61746164      /* "data" */
#define SMPL        0x6c706d73      /* "smpl" */
#define LIST        0x5453494c      /* "LIST" */
#define ID3_        0x20336469      /* "id3 " */
#define PCM_CODE    1               /* WAVE_FORMAT_PCM */
#define ADPCM_CODE  2               /* WAVE_FORMAT_ADPCM */
#define FLOAT_CODE  3               /* WAVE_FORMAT_IEEE_FLOAT */
#define ALAW_CODE   6               /* WAVE_FORMAT_ALAW */
#define uLAW_CODE   7               /* WAVE_FORMAT_MULAW */
#define EXT_CODE    0xFFFE          /* WAVE_FORMAT_EXTENSIBLE */
#define WAVE_MONO   1
#define WAVE_STEREO 2

typedef struct {
/* Not saved in the chunk we read:
    Uint32  chunkID;
    Uint32  chunkLen;
*/
    Uint16  encoding;
    Uint16  channels;       /* 1 = mono, 2 = stereo */
    Uint32  frequency;      /* One of 11025, 22050, or 44100 Hz */
    Uint32  byterate;       /* Average bytes per second */
    Uint16  blockalign;     /* Bytes per sample block */
    Uint16  bitspersample;      /* One of 8, 12, 16, or 4 for ADPCM */
} WaveFMT;

typedef struct {
    Uint32 identifier;
    Uint32 type;
    Uint32 start;
    Uint32 end;
    Uint32 fraction;
    Uint32 play_count;
} SampleLoop;

typedef struct {
/* Not saved in the chunk we read:
    Uint32  chunkID;
    Uint32  chunkLen;
*/
    Uint32  manufacturer;
    Uint32  product;
    Uint32  sample_period;
    Uint32  MIDI_unity_note;
    Uint32  MIDI_pitch_fraction;
    Uint32  SMTPE_format;
    Uint32  SMTPE_offset;
    Uint32  sample_loops;
    Uint32  sampler_data;
    SampleLoop loops[1];
} SamplerChunk;

/*********************************************/
/* Define values for AIFF (IFF audio) format */
/*********************************************/
#define FORM        0x4d524f46      /* "FORM" */
#define AIFF        0x46464941      /* "AIFF" */
#define AIFC        0x43464941      /* "AIFС" */
#define FVER        0x52455646      /* "FVER" */
#define SSND        0x444e5353      /* "SSND" */
#define COMM        0x4d4d4f43      /* "COMM" */

/* Supported compression types */
#define NONE        0x454E4F4E      /* "NONE" */
#define sowt        0x74776F73      /* "sowt" */
#define raw_        0x20776172      /* "raw " */
#define ulaw        0x77616C75      /* "ulaw" */
#define alaw        0x77616C61      /* "alaw" */
#define fl32        0x32336C66      /* "fl32" */
#define fl64        0x34366C66      /* "fl64" */

/* Function to load the WAV/AIFF stream */
static SDL_bool LoadWAVMusic(WAV_Music *wave);
static SDL_bool LoadAIFFMusic(WAV_Music *wave);

static void WAV_Delete(void *context);

static int fetch_pcm(void *context, int length);

/* Load a WAV stream from the given RWops object */
static void *WAV_CreateFromRW(SDL_RWops *src, int freesrc)
{
    WAV_Music *music;
    Uint32 magic;
    SDL_bool loaded = SDL_FALSE;

    music = (WAV_Music *)SDL_calloc(1, sizeof(*music));
    if (!music) {
        SDL_OutOfMemory();
        return NULL;
    }
    music->src = src;
    music->volume = MIX_MAX_VOLUME;
    /* Default decoder is PCM */
    music->decode = fetch_pcm;
    music->encoding = PCM_CODE;

    magic = SDL_ReadLE32(src);
    if (magic == RIFF || magic == WAVE) {
        loaded = LoadWAVMusic(music);
    } else if (magic == FORM) {
        loaded = LoadAIFFMusic(music);
    } else {
        Mix_SetError("Unknown WAVE format");
    }
    if (!loaded) {
        SDL_free(music);
        return NULL;
    }
    music->buffer = (Uint8*)SDL_malloc(music->spec.size);
    if (!music->buffer) {
        WAV_Delete(music);
        return NULL;
    }
    music->stream = SDL_NewAudioStream(
        music->spec.format, music->spec.channels, music->spec.freq,
        music_spec.format, music_spec.channels, music_spec.freq);
    if (!music->stream) {
        WAV_Delete(music);
        return NULL;
    }

    music->freesrc = (SDL_bool)freesrc;
    return music;
}

static void WAV_SetVolume(void *context, int volume)
{
    WAV_Music *music = (WAV_Music *)context;
    music->volume = volume;
}

/* Start playback of a given WAV stream */
static int WAV_Play(void *context, int play_count)
{
    WAV_Music *music = (WAV_Music *)context;
    int i;
    for (i = 0; i < music->numloops; ++i) {
        WAVLoopPoint *loop = &music->loops[i];
        loop->active = SDL_TRUE;
        loop->current_play_count = loop->initial_play_count;
    }
    music->play_count = play_count;
    if (SDL_RWseek(music->src, music->start, RW_SEEK_SET) < 0) {
        return -1;
    }
    return 0;
}


/*
    G711 source: https://github.com/escrichov/G711/blob/master/g711.c
*/

#define	SIGN_BIT	(0x80)		/* Sign bit for a A-law byte. */
#define	QUANT_MASK	(0xf)		/* Quantization field mask. */
#define	SEG_SHIFT	(4)		/* Left shift for segment number. */
#define	SEG_MASK	(0x70) /* Segment field mask. */

static Sint16 ALAW_To_PCM16(Uint8 a_val)
{
   Sint16 t;
   Sint16 seg;

   a_val ^= 0x55;

   t = (Sint16)((a_val & QUANT_MASK) << 4);
   seg = ((unsigned)a_val & SEG_MASK) >> SEG_SHIFT;
   switch (seg) {
   case 0:
      t += 8;
      break;
   case 1:
      t += 0x108;
      break;
   default:
      t += 0x108;
      t <<= seg - 1;
   }
   return ((a_val & SIGN_BIT) ? t : -t);
}

#define BIAS (0x84)		/* Bias for linear code. */
#define CLIP 8159

/*
 *
 * ulaw2linear() - Convert a u-law value to 16-bit linear PCM
 *
 * First, a biased linear code is derived from the code word. An unbiased
 * output can then be obtained by subtracting 33 from the biased code.
 *
 * Note that this function expects to be passed the complement of the
 * original code word. This is in keeping with ISDN conventions.
 */
static Sint16 uLAW_To_PCM16(Uint8 u_val)
{
   Sint16 t;
   /* Complement to obtain normal u-law value. */
   u_val = ~u_val;
   /*
    * Extract and bias the quantization bits. Then
    * shift up by the segment number and subtract out the bias.
    */
   t = (Sint16)((u_val & QUANT_MASK) << 3) + BIAS;
   t <<= ((unsigned)u_val & SEG_MASK) >> SEG_SHIFT;

   return ((u_val & SIGN_BIT) ? (BIAS - t) : (t - BIAS));
}

static int fetch_pcm(void *context, int length)
{
    WAV_Music *music = (WAV_Music *)context;
    return (int)SDL_RWread(music->src, music->buffer, 1, (size_t)length);
}

static Uint32 sign_extend_24_32(Uint32 x) {
    const Uint32 bits = 24;
    Uint32 m = 1u << (bits - 1);
    x = (SDL_SwapLE32(x) >> 8) & 0x00FFFFFF;
    return (x ^ m) - m;
}

static int fetch_pcm24be(void *context, int length)
{
    WAV_Music *music = (WAV_Music *)context;
    int i = 0, o = 0;
    length = (int)SDL_RWread(music->src, music->buffer, 1, (size_t)((length / 4) * 3));
    if (length % music->samplesize != 0) {
        length -= length % music->samplesize;
    }
    for (i = length - 1, o = ((length - 1) / 3) * 4; i >= 0; i -= 3, o -= 4) {
        Uint32 decoded = sign_extend_24_32(*((Uint32*)(music->buffer + i)));
        music->buffer[o + 0] = (decoded >> 0) & 0xFF;
        music->buffer[o + 1] = (decoded >> 8) & 0xFF;
        music->buffer[o + 2] = (decoded >> 16) & 0xFF;
        music->buffer[o + 3] = (decoded >> 24) & 0xFF;
    }
    return ((length - music->samplesize)/ 3) * 4;
}

static int fetch_xlaw(Sint16 (*decode_sample)(Uint8), void *context, int length)
{
    WAV_Music *music = (WAV_Music *)context;
    int i = 0, o = 0;
    length = (int)SDL_RWread(music->src, music->buffer, 1, (size_t)(length / 2));
    if (length % music->samplesize != 0) {
        length -= length % music->samplesize;
    }
    for (i = length - 1, o = (length - 1) * 2; i >= 0; i--, o -= 2) {
        Uint16 decoded = (Uint16)decode_sample(music->buffer[i]);
        music->buffer[o] = decoded & 0xFF;
        music->buffer[o + 1] = (decoded >> 8) & 0xFF;
    }
    return length * 2;
}

static int fetch_ulaw(void *context, int length)
{
    return fetch_xlaw(uLAW_To_PCM16, context, length);
}

static int fetch_alaw(void *context, int length)
{
    return fetch_xlaw(ALAW_To_PCM16, context, length);
}

/* Play some of a stream previously started with WAV_Play() */
static int WAV_GetSome(void *context, void *data, int bytes, SDL_bool *done)
{
    WAV_Music *music = (WAV_Music *)context;
    Sint64 pos, stop;
    WAVLoopPoint *loop;
    Sint64 loop_start = music->start;
    Sint64 loop_stop = music->stop;
    SDL_bool looped = SDL_FALSE;
    SDL_bool at_end = SDL_FALSE;
    int i;
    int filled, amount, result;

    filled = SDL_AudioStreamGet(music->stream, data, bytes);
    if (filled != 0) {
        return filled;
    }

    if (!music->play_count) {
        /* All done */
        *done = SDL_TRUE;
        return 0;
    }

    pos = SDL_RWtell(music->src);
    stop = music->stop;
    loop = NULL;
    for (i = 0; i < music->numloops; ++i) {
        loop = &music->loops[i];
        if (loop->active) {
            const int bytes_per_sample = (SDL_AUDIO_BITSIZE(music->spec.format) / 8) * music->spec.channels;
            loop_start = music->start + loop->start * (Uint32)bytes_per_sample;
            loop_stop = music->start + (loop->stop + 1) * (Uint32)bytes_per_sample;
            if (pos >= loop_start && pos < loop_stop)
            {
                stop = loop_stop;
                break;
            }
        }
        loop = NULL;
    }

    amount = (int)music->spec.size;
    if ((stop - pos) < amount) {
        amount = (int)(stop - pos);
    }

    amount = music->decode(music, amount);
    if (amount > 0) {
        result = SDL_AudioStreamPut(music->stream, music->buffer, amount);
        if (result < 0) {
            return -1;
        }
    } else {
        /* We might be looping, continue */
        at_end = SDL_TRUE;
    }

    if (loop && SDL_RWtell(music->src) >= stop) {
        if (loop->current_play_count == 1) {
            loop->active = SDL_FALSE;
        } else {
            if (loop->current_play_count > 0) {
                --loop->current_play_count;
            }
            SDL_RWseek(music->src, (Sint64)loop_start, RW_SEEK_SET);
            looped = SDL_TRUE;
        }
    }

    if (!looped && (at_end || SDL_RWtell(music->src) >= music->stop)) {
        if (music->play_count == 1) {
            music->play_count = 0;
            SDL_AudioStreamFlush(music->stream);
        } else {
            int play_count = -1;
            if (music->play_count > 0) {
                play_count = (music->play_count - 1);
            }
            if (WAV_Play(music, play_count) < 0) {
                return -1;
            }
        }
    }

    /* We'll get called again in the case where we looped or have more data */
    return 0;
}

static int WAV_GetAudio(void *context, void *data, int bytes)
{
    WAV_Music *music = (WAV_Music *)context;
    return music_pcm_getaudio(context, data, bytes, music->volume, WAV_GetSome);
}

static int WAV_Seek(void *context, double position)
{
    WAV_Music *music = (WAV_Music *)context;
    Sint64 destpos = music->start + (Sint64)(position * (double)music->spec.freq * music->samplesize);
    if (destpos > music->stop)
        return -1;
    SDL_RWseek(music->src, destpos, RW_SEEK_SET);
    return 0;
}

static double WAV_Tell(void *context)
{
    WAV_Music *music = (WAV_Music *)context;
    Sint64 phys_pos = SDL_RWtell(music->src);
    return (double)(phys_pos - music->start) / (double)(music->spec.freq * music->samplesize);
}

static double WAV_Length(void *context)
{
    WAV_Music *music = (WAV_Music *)context;
    return (double)(music->stop - music->start) / (double)(music->spec.freq * music->samplesize);
}

static const char* WAV_GetMetaTag(void *context, Mix_MusicMetaTag tag_type)
{
    WAV_Music *music = (WAV_Music *)context;
    return meta_tags_get(&music->tags, tag_type);
}

/* Close the given WAV stream */
static void WAV_Delete(void *context)
{
    WAV_Music *music = (WAV_Music *)context;

    /* Clean up associated data */
    meta_tags_clear(&music->tags);
    if (music->loops) {
        SDL_free(music->loops);
    }
    if (music->stream) {
        SDL_FreeAudioStream(music->stream);
    }
    if (music->buffer) {
        SDL_free(music->buffer);
    }
    if (music->freesrc) {
        SDL_RWclose(music->src);
    }
    SDL_free(music);
}

static SDL_bool ParseFMT(WAV_Music *wave, Uint32 chunk_length)
{
    SDL_AudioSpec *spec = &wave->spec;
    WaveFMT *format;
    Uint8 *data;
    Uint16 bitsamplerate;
    SDL_bool loaded = SDL_FALSE;

    if (chunk_length < sizeof(*format)) {
        Mix_SetError("Wave format chunk too small");
        return SDL_FALSE;
    }

    data = (Uint8 *)SDL_malloc(chunk_length);
    if (!data) {
        Mix_SetError("Out of memory");
        return SDL_FALSE;
    }
    if (!SDL_RWread(wave->src, data, chunk_length, 1)) {
        Mix_SetError("Couldn't read %d bytes from WAV file", chunk_length);
        return SDL_FALSE;
    }
    format = (WaveFMT *)data;

    wave->encoding = SDL_SwapLE16(format->encoding);
    /* Decode the audio data format */
    switch (wave->encoding) {
        case PCM_CODE:
        case FLOAT_CODE:
            /* We can understand this */
            wave->decode = fetch_pcm;
            break;
        case uLAW_CODE:
            /* , this */
            wave->decode = fetch_ulaw;
            break;
        case ALAW_CODE:
            /* , and this */
            wave->decode = fetch_alaw;
            break;
        default:
            /* but NOT this */
            Mix_SetError("Unknown WAVE data format");
            goto done;
    }
    spec->freq = (int)SDL_SwapLE32(format->frequency);
    bitsamplerate = SDL_SwapLE16(format->bitspersample);
    switch (bitsamplerate) {
        case 8:
            switch(wave->encoding) {
            case PCM_CODE:  spec->format = AUDIO_U8; break;
            case ALAW_CODE: spec->format = AUDIO_S16; break;
            case uLAW_CODE: spec->format = AUDIO_S16; break;
            default: goto unknown_length;
            }
            break;
        case 16:
            switch(wave->encoding) {
            case PCM_CODE: spec->format = AUDIO_S16; break;
            default: goto unknown_length;
            }
            break;
        case 32:
            switch(wave->encoding) {
            case PCM_CODE:   spec->format = AUDIO_S32; break;
            case FLOAT_CODE: spec->format = AUDIO_F32; break;
            default: goto unknown_length;
            }
            break;
        default:
            unknown_length:
            Mix_SetError("Unknown PCM data format of %d-bit length", (int)bitsamplerate);
            goto done;
    }
    spec->channels = (Uint8) SDL_SwapLE16(format->channels);
    spec->samples = 4096;       /* Good default buffer size */
    wave->samplesize = spec->channels * (bitsamplerate / 8);
    /* SDL_CalculateAudioSpec */
    spec->size = SDL_AUDIO_BITSIZE(spec->format) / 8;
    spec->size *= spec->channels;
    spec->size *= spec->samples;

    loaded = SDL_TRUE;

done:
    SDL_free(data);
    return loaded;
}

static SDL_bool ParseDATA(WAV_Music *wave, Uint32 chunk_length)
{
    wave->start = SDL_RWtell(wave->src);
    wave->stop = wave->start + chunk_length;
    SDL_RWseek(wave->src, chunk_length, RW_SEEK_CUR);
    return SDL_TRUE;
}

static SDL_bool AddLoopPoint(WAV_Music *wave, Uint32 play_count, Uint32 start, Uint32 stop)
{
    WAVLoopPoint *loop;
    WAVLoopPoint *loops = SDL_realloc(wave->loops, (size_t)(wave->numloops + 1) * sizeof(*wave->loops));
    if (!loops) {
        Mix_SetError("Out of memory");
        return SDL_FALSE;
    }

    loop = &loops[ wave->numloops ];
    loop->start = start;
    loop->stop = stop;
    loop->initial_play_count = play_count;
    loop->current_play_count = play_count;

    wave->loops = loops;
    ++wave->numloops;
    return SDL_TRUE;
}

static SDL_bool ParseSMPL(WAV_Music *wave, Uint32 chunk_length)
{
    SamplerChunk *chunk;
    Uint8 *data;
    Uint32 i;
    SDL_bool loaded = SDL_FALSE;

    data = (Uint8 *)SDL_malloc(chunk_length);
    if (!data) {
        Mix_SetError("Out of memory");
        return SDL_FALSE;
    }
    if (!SDL_RWread(wave->src, data, chunk_length, 1)) {
        Mix_SetError("Couldn't read %d bytes from WAV file", chunk_length);
        return SDL_FALSE;
    }
    chunk = (SamplerChunk *)data;

    for (i = 0; i < SDL_SwapLE32(chunk->sample_loops); ++i) {
        const Uint32 LOOP_TYPE_FORWARD = 0;
        Uint32 loop_type = SDL_SwapLE32(chunk->loops[i].type);
        if (loop_type == LOOP_TYPE_FORWARD) {
            AddLoopPoint(wave, SDL_SwapLE32(chunk->loops[i].play_count), SDL_SwapLE32(chunk->loops[i].start), SDL_SwapLE32(chunk->loops[i].end));
        }
    }

    loaded = SDL_TRUE;
    SDL_free(data);
    return loaded;
}

static void read_meta_field(Mix_MusicMetaTags *tags, Mix_MusicMetaTag tag_type, size_t *i, Uint32 chunk_length, Uint8 *data, size_t fieldOffset)
{
    Uint32 len = 0;
    int isID3 = fieldOffset == 7;
    char *field = NULL;
    *i += 4;
    len = isID3 ?
          SDL_SwapBE32(*((Uint32 *)(data + *i))) : /* ID3  */
          SDL_SwapLE32(*((Uint32 *)(data + *i))); /* LIST */
    if (len > chunk_length) {
        return; /* Do nothing due to broken lenght */
    }
    *i += fieldOffset;
    field = (char *)SDL_malloc(len + 1);
    SDL_memset(field, 0, (len + 1));
    SDL_strlcpy(field, (char *)(data + *i), isID3 ? len - 1 : len);
    *i += len;
    meta_tags_set(tags, tag_type, field);
    SDL_free(field);
}

static SDL_bool ParseLIST(WAV_Music *wave, Uint32 chunk_length)
{
    SDL_bool loaded = SDL_FALSE;

    Uint8 *data;
    data = (Uint8 *)SDL_malloc(chunk_length);
    if (!data) {
        Mix_SetError("Out of memory");
        return SDL_FALSE;
    }

    if (!SDL_RWread(wave->src, data, chunk_length, 1)) {
        Mix_SetError("Couldn't read %d bytes from WAV file", chunk_length);
        return SDL_FALSE;
    }

    if (SDL_strncmp((char *)data, "INFO", 4) == 0) {
        size_t i = 4;
        for (i = 4; i < chunk_length - 4;) {
            if(SDL_strncmp((char *)(data + i), "INAM", 4) == 0) {
                read_meta_field(&wave->tags, MIX_META_TITLE, &i, chunk_length, data, 4);
                continue;
            } else if(SDL_strncmp((char *)(data + i), "IART", 4) == 0) {
                read_meta_field(&wave->tags, MIX_META_ARTIST, &i, chunk_length, data, 4);
                continue;
            } else if(SDL_strncmp((char *)(data + i), "IALB", 4) == 0) {
                read_meta_field(&wave->tags, MIX_META_ALBUM, &i, chunk_length, data, 4);
                continue;
            } else if (SDL_strncmp((char *)(data + i), "BCPR", 4) == 0) {
                read_meta_field(&wave->tags, MIX_META_COPYRIGHT, &i, chunk_length, data, 4);
                continue;
            }
            i++;
        }
        loaded = SDL_TRUE;
    }

    /* done: */
    SDL_free(data);

    return loaded;
}

static SDL_bool ParseID3(WAV_Music *wave, Uint32 chunk_length)
{
    SDL_bool loaded = SDL_FALSE;

    Uint8 *data;
    data = (Uint8 *)SDL_malloc(chunk_length);

    if (!data) {
        Mix_SetError("Out of memory");
        return SDL_FALSE;
    }

    if (!SDL_RWread(wave->src, data, chunk_length, 1)) {
        Mix_SetError("Couldn't read %d bytes from WAV file", chunk_length);
        return SDL_FALSE;
    }

    if (SDL_strncmp((char *)data, "ID3", 3) == 0) {
        id3tag_fetchTagsFromMemory(&wave->tags, data, chunk_length);
        loaded = SDL_TRUE;
    }

    /* done: */
    SDL_free(data);

    return loaded;
}

static SDL_bool LoadWAVMusic(WAV_Music *wave)
{
    SDL_RWops *src = wave->src;
    Uint32 chunk_type;
    Uint32 chunk_length;
    SDL_bool found_FMT = SDL_FALSE;
    SDL_bool found_DATA = SDL_FALSE;

    /* WAV magic header */
    Uint32 wavelen;
    Uint32 WAVEmagic;
    MIX_UNUSED(WAVEmagic);
    MIX_UNUSED(wavelen);

    meta_tags_init(&wave->tags);

    /* Check the magic header */
    wavelen = SDL_ReadLE32(src);
    WAVEmagic = SDL_ReadLE32(src);

    /* Read the chunks */
    for (; ;) {
        chunk_type = SDL_ReadLE32(src);
        chunk_length = SDL_ReadLE32(src);

        if (chunk_length == 0)
            break;

        switch (chunk_type)
        {
        case FMT:
            found_FMT = SDL_TRUE;
            if (!ParseFMT(wave, chunk_length))
                return SDL_FALSE;
            break;
        case DATA:
            found_DATA = SDL_TRUE;
            if (!ParseDATA(wave, chunk_length))
                return SDL_FALSE;
            break;
        case SMPL:
            if (!ParseSMPL(wave, chunk_length))
                return SDL_FALSE;
            break;
        case LIST:
            if (!ParseLIST(wave, chunk_length))
                return SDL_FALSE;
            break;
        case ID3_:
            if (!ParseID3(wave, chunk_length))
                return SDL_FALSE;
            break;
        default:
            SDL_RWseek(src, chunk_length, RW_SEEK_CUR);
            break;
        }
    }

    if (!found_FMT) {
        Mix_SetError("Bad WAV file (no FMT chunk)");
        return SDL_FALSE;
    }

    if (!found_DATA) {
        Mix_SetError("Bad WAV file (no DATA chunk)");
        return SDL_FALSE;
    }

    return SDL_TRUE;
}

/* I couldn't get SANE_to_double() to work, so I stole this from libsndfile.
 * I don't pretend to fully understand it.
 */

static Uint32 SANE_to_Uint32 (Uint8 *sanebuf)
{
    /* Negative number? */
    if (sanebuf[0] & 0x80)
        return 0;

    /* Less than 1? */
    if (sanebuf[0] <= 0x3F)
        return 1;

    /* Way too big? */
    if (sanebuf[0] > 0x40)
        return 0x4000000;

    /* Still too big? */
    if (sanebuf[0] == 0x40 && sanebuf[1] > 0x1C)
        return 800000000;

    return (Uint32)(((sanebuf[2] << 23) | (sanebuf[3] << 15) | (sanebuf[4] << 7) |
            (sanebuf[5] >> 1)) >> (29 - sanebuf[1]));
}

static SDL_bool LoadAIFFMusic(WAV_Music *wave)
{
    SDL_RWops *src = wave->src;
    SDL_AudioSpec *spec = &wave->spec;
    SDL_bool found_SSND = SDL_FALSE;
    SDL_bool found_COMM = SDL_FALSE;
    SDL_bool found_FVER = SDL_FALSE;
    SDL_bool is_AIFC = SDL_FALSE;

    Uint32 chunk_type;
    Uint32 chunk_length;
    Sint64 next_chunk;

    /* AIFF magic header */
    Uint32 AIFFmagic;
    /* SSND chunk        */
    Uint32 offset;
    Uint32 blocksize;
    /* COMM format chunk */
    Uint16 channels = 0;
    Uint32 numsamples = 0;
    Uint16 samplesize = 0;
    Uint8 sane_freq[10];
    Uint32 frequency = 0;
    Uint32 AIFCVersion1 = 0;
    Uint32 compressionType = 0;

    MIX_UNUSED(blocksize);
    MIX_UNUSED(AIFCVersion1);

    /* Check the magic header */
    chunk_length = SDL_ReadBE32(src);
    AIFFmagic = SDL_ReadLE32(src);
    if (AIFFmagic != AIFF && AIFFmagic != AIFC) {
        Mix_SetError("Unrecognized file type (not AIFF or AIFC)");
        return SDL_FALSE;
    }
    if (AIFFmagic == AIFC) {
        is_AIFC = SDL_TRUE;
    }

    /* From what I understand of the specification, chunks may appear in
     * any order, and we should just ignore unknown ones.
     *
     * TODO: Better sanity-checking. E.g. what happens if the AIFF file
     *       contains compressed sound data?
     */
    do {
        chunk_type      = SDL_ReadLE32(src);
        chunk_length    = SDL_ReadBE32(src);
        next_chunk      = SDL_RWtell(src) + chunk_length;

        /* Paranoia to avoid infinite loops */
        if (chunk_length == 0)
            break;

        switch (chunk_type) {
        case SSND:
            found_SSND = SDL_TRUE;
            offset = SDL_ReadBE32(src);
            blocksize = SDL_ReadBE32(src);
            wave->start = SDL_RWtell(src) + offset;
            break;
        case FVER:
            found_FVER = SDL_TRUE;
            AIFCVersion1 = SDL_ReadBE32(src);
            break;

        case COMM:
            found_COMM = SDL_TRUE;

            /* Read the audio data format chunk */
            channels = SDL_ReadBE16(src);
            numsamples = SDL_ReadBE32(src);
            samplesize = SDL_ReadBE16(src);
            SDL_RWread(src, sane_freq, sizeof(sane_freq), 1);
            frequency = SANE_to_Uint32(sane_freq);
            if (is_AIFC) {
                compressionType = SDL_ReadLE32(src);
                /* here must be a "compressionName" which is a padded string */
            }
            break;

        default:
            break;
        }
    } while ((!found_SSND || !found_COMM || (is_AIFC && !found_FVER))
         && SDL_RWseek(src, next_chunk, RW_SEEK_SET) != -1);

    if (!found_SSND) {
        Mix_SetError("Bad AIFF file (no SSND chunk)");
        return SDL_FALSE;
    }

    if (!found_COMM) {
        Mix_SetError("Bad AIFF file (no COMM chunk)");
        return SDL_FALSE;
    }

    wave->samplesize = channels * (samplesize / 8);
    wave->stop = wave->start + channels * numsamples * (samplesize / 8);

    /* Decode the audio data format */
    SDL_memset(spec, 0, (sizeof *spec));
    spec->freq = (int)frequency;
    switch (samplesize) {
        case 8:
            if (!is_AIFC)
                spec->format = AUDIO_S8;
            else switch (compressionType) {
            case raw_: spec->format = AUDIO_U8; break;
            case sowt: spec->format = AUDIO_S8; break;
            case ulaw:
                spec->format = AUDIO_S16LSB;
                wave->encoding = uLAW_CODE;
                wave->decode = fetch_ulaw;
                break;
            case alaw:
                spec->format = AUDIO_S16LSB;
                wave->encoding = ALAW_CODE;
                wave->decode = fetch_alaw;
                break;
            default: goto unsupported_format;
            }
            break;
        case 16:
            if (!is_AIFC)
                spec->format = AUDIO_S16MSB;
            else switch (compressionType) {
            case sowt: spec->format = AUDIO_S16LSB; break;
            case NONE: spec->format = AUDIO_S16MSB; break;
            default: goto unsupported_format;
            }
            break;
        case 24:
            wave->encoding = PCM_CODE;
            wave->decode = fetch_pcm24be;
            if (!is_AIFC)
                spec->format = AUDIO_S32MSB;
            else switch (compressionType) {
            case sowt: spec->format = AUDIO_S32LSB; break;
            case NONE: spec->format = AUDIO_S32MSB; break;
            default: goto unsupported_format;
            }
            break;
        case 32:
            if (!is_AIFC)
                spec->format = AUDIO_S32MSB;
            else switch (compressionType) {
            case sowt: spec->format = AUDIO_S32LSB; break;
            case NONE: spec->format = AUDIO_S32MSB; break;
            case fl32: spec->format = AUDIO_F32MSB; break;
            default: goto unsupported_format;
            }
            break;
        default:
        unsupported_format:
            Mix_SetError("Unknown samplesize in data format");
            return SDL_FALSE;
    }
    spec->channels = (Uint8) channels;
    spec->samples = 4096;       /* Good default buffer size */
    spec->size = SDL_AUDIO_BITSIZE(spec->format) / 8;
    spec->size *= spec->channels;
    spec->size *= spec->samples;

    return SDL_TRUE;
}

Mix_MusicInterface Mix_MusicInterface_WAV =
{
    "WAVE",
    MIX_MUSIC_WAVE,
    MUS_WAV,
    SDL_FALSE,
    SDL_FALSE,

    NULL,   /* Load */
    NULL,   /* Open */
    WAV_CreateFromRW,
    NULL,   /* CreateFromRWex [MIXER-X]*/
    NULL,   /* CreateFromFile */
    NULL,   /* CreateFromFileEx [MIXER-X]*/
    WAV_SetVolume,
    WAV_Play,
    NULL,   /* IsPlaying */
    WAV_GetAudio,
    WAV_Seek,   /* Seek */
    WAV_Tell,   /* Tell [MIXER-X]*/
    WAV_Length, /* FullLength [MIXER-X]*/
    NULL,   /* LoopStart [MIXER-X]*/
    NULL,   /* LoopEnd [MIXER-X]*/
    NULL,   /* LoopLength [MIXER-X]*/
    WAV_GetMetaTag,   /* GetMetaTag [MIXER-X]*/
    NULL,   /* Pause */
    NULL,   /* Resume */
    NULL,   /* Stop */
    WAV_Delete,
    NULL,   /* Close */
    NULL,   /* Unload */
};

#endif /* MUSIC_WAV */

/* vi: set ts=4 sw=4 expandtab: */
