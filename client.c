#include "miniaudio.h"
#include "socket.h"
#define SW_SOCKET_IMPLEMENTATION
#include "socket.h"

#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

#include <vorbis/vorbisenc.h>

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>

#define PASSWORD_LEN 64
#define PASSWORD "125b69e01a6ecb38220b2fd425201f08e6950f09e6daaaf914b26718b88d09ab"

#define CHUNK_SIZE (512) // frames

#define SAMPLE_RATE (48000)
#define CAPTURE_CHANNELS (2)
#define CAPTURE_FORMAT (ma_format_f32)

//#define RELAY_IP "65.21.200.18"
#define RELAY_PORT (30001)

#define DEBUG_FILE

#ifdef DEBUG_FILE
FILE* g_debugfile = NULL;
#endif

typedef struct {
    char password[PASSWORD_LEN];
    uint32_t ogg_headers_size;
} stream_handshake_t;

typedef struct {
    float l[CHUNK_SIZE];
    float r[CHUNK_SIZE];
} stereo_chunk_t;

typedef struct ogg_chunk {
    struct ogg_chunk* next;
    size_t size;
    uint8_t* data;
} ogg_chunk_t;

struct {
    vorbis_info info;
    ogg_stream_state stream; /* take physical pages, weld into a logical stream of packets */
    vorbis_dsp_state vd; /* central working state for the packet->PCM decoder */
    vorbis_block block; /* local working space for packet->PCM decode */

    ogg_chunk_t* chunks_head;
    ogg_chunk_t* chunks_tail;

    //size_t header_size;
    stream_handshake_t handshake;
    uint8_t header_buf[64 * 1024];
} ogg;

static const uint8_t* ogg_peek(size_t* size)
{
    if (ogg.chunks_head == NULL)
    {
        *size = 0;
        return NULL;
    }

    *size = ogg.chunks_head->size;
    return ogg.chunks_head->data;
}

static void ogg_pop(void)
{
    assert(ogg.chunks_tail != NULL);
    ogg_chunk_t* popped = ogg.chunks_tail;
    ogg.chunks_tail = popped->next;

    free(popped->data);

    if (ogg.chunks_tail == NULL)
    {
        assert(ogg.chunks_head == popped);
        ogg.chunks_head = NULL;
    }
}

static int ogg__append_header(const ogg_page* page)
{
    size_t total_size = page->header_len + page->body_len;

    memcpy(ogg.header_buf + ogg.handshake.ogg_headers_size, page->header, page->header_len);
    memcpy(ogg.header_buf + ogg.handshake.ogg_headers_size + page->header_len, page->body, page->body_len);

#ifdef DEBUG_FILE
    fwrite(ogg.header_buf + ogg.handshake.ogg_headers_size, 1, total_size, g_debugfile);
#endif

    ogg.handshake.ogg_headers_size += total_size;

    return 0;
}

static int ogg__append(const ogg_page* page)
{
    ogg_chunk_t* chunk = malloc(sizeof(ogg_chunk_t));
    assert(chunk != NULL);
    chunk->next = NULL;
    chunk->size = page->header_len + page->body_len;
    chunk->data = malloc(chunk->size);
    assert(chunk->data != NULL);
    memcpy(chunk->data, page->header, page->header_len);
    memcpy(chunk->data + page->header_len, page->body, page->body_len);

#ifdef DEBUG_FILE
    fwrite(chunk->data, 1, chunk->size, g_debugfile);
#endif

    if (ogg.chunks_head != NULL)
    {
        ogg.chunks_head->next = chunk;
    }

    ogg.chunks_head = chunk;

    if (ogg.chunks_tail == NULL)
    {
        ogg.chunks_tail = chunk;
    }

    return 0;
}

static int ogg__append_headers()
{
    vorbis_comment comment;
    vorbis_comment_init(&comment);
    vorbis_comment_add_tag(&comment, "ENCODER", "SYNTHESIZER");
    vorbis_comment_add_tag(&comment, "ARTIST", "SYNTHESIZER");

    /* Vorbis streams begin with three headers; the initial header (with
    most of the codec setup parameters) which is mandated by the Ogg
    bitstream spec.  The second header holds any comment fields.  The
    third header holds the bitstream codebook.  We merely need to
    make the headers, then pass them to libvorbis one at a time;
    libvorbis handles the additional Ogg bitstream constraints */

    {
        ogg_packet header;
        ogg_packet header_comm;
        ogg_packet header_code;

        vorbis_analysis_headerout(&ogg.vd, &comment, &header, &header_comm, &header_code);
        ogg_stream_packetin(&ogg.stream, &header); /* automatically placed in its own page */
        ogg_stream_packetin(&ogg.stream, &header_comm);
        ogg_stream_packetin(&ogg.stream, &header_code);

        /* This ensures the actual audio data will start on a new page, as per spec */
        for (;;)
        {
            ogg_page page;
            int result = ogg_stream_flush(&ogg.stream, &page);
            if (result == 0) break;
            ogg__append_header(&page);
        }
    }

    return 0;
}

static int ogg_init(void)
{
    int ret;

    vorbis_info_init(&ogg.info);

    ret = vorbis_encode_init_vbr(&ogg.info, CAPTURE_CHANNELS, SAMPLE_RATE, 0.1);

    if (ret)
    {
        fprintf(stderr, "vorbis_encode_init_vbr failed.\n");
        return 1;
    }

    vorbis_analysis_init(&ogg.vd, &ogg.info);
    vorbis_block_init(&ogg.vd, &ogg.block);
    ogg_stream_init(&ogg.stream, 0xdeadbeef);

    ogg__append_headers();
    return 0;
}

static int ogg_encode(stereo_chunk_t* pcm_chunk)
{
    int eos = 0;

    float** vorbis_buffer = vorbis_analysis_buffer(&ogg.vd, CHUNK_SIZE);
    memcpy(vorbis_buffer[0], pcm_chunk->l, CHUNK_SIZE * sizeof(float));
    memcpy(vorbis_buffer[1], pcm_chunk->r, CHUNK_SIZE * sizeof(float));

    vorbis_analysis_wrote(&ogg.vd, CHUNK_SIZE);

    /* vorbis does some data preanalysis, then divvies up blocks for
    more involved (potentially parallel) processing.  Get a single
    block for encoding now */
    while (vorbis_analysis_blockout(&ogg.vd, &ogg.block) == 1)
    {
        /* analysis, assume we want to use bitrate management */
        vorbis_analysis(&ogg.block,NULL);
        vorbis_bitrate_addblock(&ogg.block);

        ogg_packet packet; /* one raw packet of data for decode */
        while(vorbis_bitrate_flushpacket(&ogg.vd,&packet))
        {
            /* weld the packet into the bitstream */
            ogg_stream_packetin(&ogg.stream,&packet);

            /* write out pages (if any) */
            while(!eos){
                ogg_page page;
                int result = ogg_stream_pageout(&ogg.stream, &page);
                if (result == 0) break;
                ogg__append(&page);

                /* this could be set above, but for illustrative purposes, I do
                it here (to show that vorbis does know where the stream ends) */
                if (ogg_page_eos(&page)) eos = 1;
            }
        }
    }

    return 0;
}

struct {
    ma_format format;
    ma_uint32 channels;
    size_t chunk_cursor;
    stereo_chunk_t chunk;
} ma;

static void pcm_deinterleave(float* out_l, float* out_r, const float* in_lr, size_t count)
{
    for (size_t i = 0; i < count; ++i)
    {
        out_l[i] = in_lr[i * 2 + 0];
        out_r[i] = in_lr[i * 2 + 1];
    }
}

static float t = 0.0f;

static void capture_callback(ma_device* pDevice, void* pOutput, const void* pInput, ma_uint32 frame_count)
{
    (void)pDevice;
    (void)pOutput;

    const float* input_lr = pInput;

    while (frame_count > 0)
    {
        assert(ma.chunk_cursor < CHUNK_SIZE);
        const size_t chunk_remaining = CHUNK_SIZE - ma.chunk_cursor;
        size_t count = frame_count;
        if (count > chunk_remaining) count = chunk_remaining;

        pcm_deinterleave(
            ma.chunk.l + ma.chunk_cursor,
            ma.chunk.r + ma.chunk_cursor,
            input_lr,
            count);

        ma.chunk_cursor += count;
        frame_count -= count;
        input_lr += count * CAPTURE_CHANNELS;

        if (ma.chunk_cursor == CHUNK_SIZE)
        {
            ogg_encode(&ma.chunk);
            ma.chunk_cursor = 0;
        }
    }
}

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        printf("Usage:\n");
        printf("blabla\n");
        return 1;
    }

#ifdef DEBUG_FILE
    g_debugfile = fopen("debug.ogg", "wb");
#endif

    if (ogg_init() != 0) {
        fprintf(stderr, "Failed to initialize libvorbis");
        return 1;
    }

    sw_init();

    // connect to example.org (IPv4: 93.184.216.34) port 80
    //sw_socket s = sw_connect("65.21.200.18", 80);
    
    //sw_socket s = sw_socket_create();
    //sw_result r = sw_connect(s, argv[1], RELAY_PORT);
    //assert(r == SW_OK);

    sw_socket s;
    sw_result r;

    ma_result result;
    ma_context context;
    ma_device_config deviceConfig;
    ma_device device;

    result = ma_context_init(NULL, 0, NULL, &context);
    if (result != MA_SUCCESS) { fprintf(stderr, "Failed to init context\n"); return 1; }

    deviceConfig = ma_device_config_init(ma_device_type_capture);
    deviceConfig.capture.format   = CAPTURE_FORMAT;
    deviceConfig.capture.channels = CAPTURE_CHANNELS;
    deviceConfig.sampleRate       = SAMPLE_RATE;
    deviceConfig.dataCallback     = capture_callback;
    deviceConfig.pUserData        = NULL;

    result = ma_device_init(&context, &deviceConfig, &device);
    if (result != MA_SUCCESS) { fprintf(stderr, "Failed to open capture device\n"); return 1; }

    result = ma_device_start(&device);
    if (result != MA_SUCCESS) { fprintf(stderr, "Failed to start device\n"); ma_device_uninit(&device); return 1; }

    enum {
        STREAM_DISCONNECTED = 0,
        STREAM_HANDSHAKE,
        STREAM_HEADERS,
        STREAM_CHUNKS,
    } state = STREAM_DISCONNECTED;
    const uint8_t* pending_ogg = NULL;
    size_t pending_ogg_size = 0;
    size_t ogg_pos = 0;

    for (;;)
    {
        switch (state)
        {
            case STREAM_DISCONNECTED:
            {
                size_t dummy;
                if (ogg_peek(&dummy) != NULL)
                {
                    ogg_pop();
                }

                s = sw_socket_create();
                //sw_socket_set_nonblocking(s, 1);
                r = sw_connect(s, argv[1], RELAY_PORT);
                if (r == SW_OK || r == SW_WOULD_BLOCK)
                {
                    state = STREAM_HANDSHAKE;
                    break;
                }

                sw_socket_close(s);
                s = SW_INVALID_SOCKET;
                break;
            }
            case STREAM_HANDSHAKE:
            {
                memcpy(ogg.handshake.password, PASSWORD, PASSWORD_LEN);
                size_t nsent;
                r = sw_send(s, &ogg.handshake, sizeof(ogg.handshake), &nsent);
                if (r == SW_WOULD_BLOCK) break;
                if (r == SW_ERR)
                {
                    state = STREAM_DISCONNECTED;
                    break;
                }
                assert(r == SW_OK);
                assert(nsent == sizeof(ogg.handshake));
                pending_ogg = ogg.header_buf;
                pending_ogg_size = ogg.handshake.ogg_headers_size;
                state = STREAM_HEADERS;
                break;
            }
            case STREAM_HEADERS:
            {
                const size_t ogg_remaining = pending_ogg_size - ogg_pos;

                size_t nsent;
                r = sw_send(s, pending_ogg + ogg_pos, ogg_remaining, &nsent);
                if (r == SW_WOULD_BLOCK) break;
                assert(r == SW_OK);

                printf("nsent = %zu\n", nsent);

                ogg_pos += nsent;
                if (ogg_pos == pending_ogg_size)
                {
                    ogg_pos = 0;
                    pending_ogg = NULL;
                    pending_ogg_size = 0;
                    state = STREAM_CHUNKS;
                    break;
                }
                break;
            }
            case STREAM_CHUNKS:
            {
                if (pending_ogg == NULL)
                {
                    pending_ogg = ogg_peek(&pending_ogg_size);
                    if (pending_ogg == NULL)
                    {
                        ma_sleep(1);
                        continue;
                    }

                    ogg_pos = 0;
                }
                else if (ogg_pos < pending_ogg_size)
                {
                    const size_t ogg_remaining = pending_ogg_size - ogg_pos;

                    size_t nsent;
                    r = sw_send(s, pending_ogg + ogg_pos, ogg_remaining, &nsent);
                    if (r == SW_WOULD_BLOCK) break;
                    if (r != SW_OK)
                    {
                        printf("disconnected!\n");
                        state = STREAM_DISCONNECTED;
                        break;
                    }

                    printf("nsent = %zu\n", nsent);

                    ogg_pos += nsent;
                }
                else
                {
                    pending_ogg = NULL;
                    ogg_pop();
                }

                break;
            }
        }
    }

    ma_device_uninit(&device);
    ma_context_uninit(&context);
    return 0;
}