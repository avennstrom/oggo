#include "socket.h"
#define SW_SOCKET_IMPLEMENTATION
#include "socket.h"
#include "llhttp/llhttp.h"

#include <assert.h>
#include <stdio.h>

#define MAX_CLIENTS (1024)
#define PASSWORD "125b69e01a6ecb38220b2fd425201f08e6950f09e6daaaf914b26718b88d09ab"
#define BUFFER_SIZE (64 * 1024 * 1024)
#define OGG_HEADER_BUF_SIZE (64 * 1024)

#define LISTENER_PORT (30000)
#define STREAMER_PORT (30001)

#define HTTP_CHUNK_COUNT (1024)

#ifdef _MSC_VER
#define BIND_ADDRESS "127.0.0.1"
#else
#define BIND_ADDRESS "0.0.0.0"
#endif

typedef struct {
	char password[64];
	uint32_t ogg_headers_size;
} stream_handshake_t;

static inline unsigned _ctz64(unsigned long long x)
{
#ifdef _MSC_VER
	unsigned long idx;
	_BitScanForward64(&idx, x);
	return (unsigned)idx;
#else
	return __builtin_ctzll(x);
#endif
}

static const char g_http_response[] = 
	"HTTP/1.1 200 OK\r\n"
	"Content-Type: audio/ogg\r\n"
	"Transfer-Encoding: chunked\r\n"
	"Connection: keep-alive\r\n"
	"Cache-Control: no-cache\r\n"
	"\r\n";
static size_t g_http_response_len = sizeof(g_http_response) - 1;

enum
{
	CLIENT_READ_HTTP_REQUEST = 0,
	//CLIENT_AWAIT_STREAMER,
	CLIENT_SEND_HTTP_HEADERS,
	CLIENT_SEND_OGG_HEADERS,
	CLIENT_SEND_HTTP_CHUNK_HEADER,
	CLIENT_SEND_HTTP_CHUNK_BODY,
};
typedef struct
{
	uint8_t state;
	sw_socket s;
	uint8_t http_tail;
	size_t http_chunk_tail;
	size_t http_chunk_cursor;
	size_t ogg_headers_cursor;
	llhttp_t http_parser;
} client_t;

static client_t clients[MAX_CLIENTS];
static uint64_t client_alloc_mask[MAX_CLIENTS / 64];

typedef struct
{
	size_t size;
	const uint8_t* data;
} http_chunk_t;

static size_t http_chunk_head = 0;
static http_chunk_t http_chunks[HTTP_CHUNK_COUNT];

static size_t ogg_header_hexlen_size = 0;
static size_t ogg_header_buf_pos = 0;
static uint8_t ogg_header_buf[OGG_HEADER_BUF_SIZE];
static size_t ogg_buf_head = 0;
static uint8_t ogg_buf[BUFFER_SIZE];

static llhttp_settings_t llhttp_settings;

static size_t client_alloc(void)
{
	for (size_t i = 0; i < MAX_CLIENTS / 64; ++i)
	{
		const uint64_t mask = ~client_alloc_mask[i];
		if (mask)
		{
			const size_t bit = _ctz64(mask);
			client_alloc_mask[i] |= (1ull << bit);

			return i * 64 + bit;
		}
	}

	return SIZE_MAX;
}

static int client__llhttp_on_message_complete(llhttp_t* parser)
{
	client_t* client = parser->data;
	client->state = CLIENT_SEND_HTTP_HEADERS;
	return 0;
}

int main(int argc, char** argv)
{
	sw_result r;

	sw_init();

	sw_socket broadcast_socket = sw_socket_create();
	sw_socket streamer_server_socket = sw_socket_create();

	sw_socket_set_nonblocking(broadcast_socket, 1);
	sw_socket_set_nonblocking(streamer_server_socket, 1);

	sw_bind(broadcast_socket, BIND_ADDRESS, LISTENER_PORT);
	sw_listen(broadcast_socket, 64);
	printf("Listening for listeners on port %d\n", LISTENER_PORT);
	sw_bind(streamer_server_socket, BIND_ADDRESS, STREAMER_PORT);
	sw_listen(streamer_server_socket, 1);
	printf("Listening for streamer on port %d\n", STREAMER_PORT);

	enum {
		STREAMER_HANDSHAKE = 0,
		STREAMER_OGG_HEADERS,
		STREAMER_STREAM,
	} streamer_state = STREAMER_HANDSHAKE;
	sw_socket streamer_socket = SW_INVALID_SOCKET;
	stream_handshake_t streamer_handshake = {0};

	llhttp_settings_init(&llhttp_settings);
	llhttp_settings.on_message_complete = client__llhttp_on_message_complete;

	for (;;)
	{
		sw_socket accepted_socket;
		r = sw_accept(broadcast_socket, &accepted_socket);
		if (r == SW_OK)
		{
			const size_t client_index = client_alloc();
			printf("listener connected: %zu\n", client_index);

			sw_socket_set_nonblocking(accepted_socket, 1);

			client_t* client = &clients[client_index];
			client->s = accepted_socket;
			client->state = CLIENT_READ_HTTP_REQUEST;
			client->http_tail = 0;
			client->http_chunk_cursor = 0;
			client->http_chunk_tail = http_chunk_head;
			client->ogg_headers_cursor = 0;
			llhttp_init(&client->http_parser, HTTP_REQUEST, &llhttp_settings);
			client->http_parser.data = client;
			//llhttp_reset(&client->http_parser);
		}

		if (streamer_socket == SW_INVALID_SOCKET)
		{
			r = sw_accept(streamer_server_socket, &streamer_socket);
			if (r == SW_OK)
			{
				printf("streamer connected\n");
				sw_socket_set_nonblocking(streamer_socket, 1);
				ogg_header_buf_pos = 0;
				http_chunk_head = 0;
				streamer_state = STREAMER_HANDSHAKE;
			}
		}

		if (streamer_socket != SW_INVALID_SOCKET)
		{
			switch (streamer_state)
			{
				case STREAMER_HANDSHAKE:
				{
					size_t nread;
					r = sw_recv(streamer_socket, &streamer_handshake, sizeof(streamer_handshake), &nread);
					if (r == SW_WOULD_BLOCK) break;
					if (r != SW_OK)
					{
						printf("streamer disconnected!\n");
						streamer_socket = SW_INVALID_SOCKET;
						break;
					}
					assert(nread == sizeof(streamer_handshake));

					if (memcmp(streamer_handshake.password, PASSWORD, sizeof(streamer_handshake.password)) != 0)
					{
						sw_socket_close(streamer_socket);
						break;
					}

					printf("handshake (ogg_headers_size = %u)\n", streamer_handshake.ogg_headers_size);

					ogg_header_hexlen_size = sprintf(ogg_header_buf + ogg_header_buf_pos, "%X\r\n", streamer_handshake.ogg_headers_size);
					ogg_header_buf_pos += ogg_header_hexlen_size;

					streamer_state = STREAMER_OGG_HEADERS;
					break;
				}
				case STREAMER_OGG_HEADERS:
				{
					size_t nread;
					r = sw_recv(streamer_socket, ogg_header_buf + ogg_header_buf_pos, ogg_header_hexlen_size + streamer_handshake.ogg_headers_size - ogg_header_buf_pos, &nread);
					if (r == SW_WOULD_BLOCK) break;
					ogg_header_buf_pos += nread;
					if (ogg_header_buf_pos == ogg_header_hexlen_size + streamer_handshake.ogg_headers_size)
					{
						ogg_header_buf_pos += sprintf(ogg_header_buf + ogg_header_buf_pos, "\r\n", 2);
						streamer_state = STREAMER_STREAM;
						break;
					}
					break;
				}
				case STREAMER_STREAM:
				{
					size_t nread;
					r = sw_recv(streamer_socket, ogg_buf + ogg_buf_head, 64 * 1024, &nread);
					if ((r == SW_OK || r == SW_WOULD_BLOCK) && nread > 0)
					{
						printf("nread = %zu, http_chunk_head = %zu\n", nread, http_chunk_head);

						http_chunks[http_chunk_head++] = (http_chunk_t){
							.size = nread,
							.data = ogg_buf + ogg_buf_head,
						};

						ogg_buf_head += nread;
					}
					else if (r == SW_ERR || r == SW_CLOSED)
					{
						printf("streamer disconnected!\n");
						streamer_socket = SW_INVALID_SOCKET;
						break;
					}
				}
				break;
			}
		}

		if (streamer_socket == SW_INVALID_SOCKET)
		{
			for (size_t i = 0; i < MAX_CLIENTS / 64; ++i)
			{
				uint64_t mask = client_alloc_mask[i];
				while (mask)
				{
					const size_t bit = _ctz64(mask);
					mask &= ~(1ull << bit);
					const size_t index = i * 64 + bit;
					client_t* client = &clients[index];

					sw_socket_close(client->s);
					client->s = SW_INVALID_SOCKET;

					printf("listener %zu closed\n", index);
				}

				client_alloc_mask[i] = 0;
			}
		}
		else
		{
			for (size_t i = 0; i < MAX_CLIENTS / 64; ++i)
			{
				uint64_t mask = client_alloc_mask[i];
				while (mask)
				{
					const size_t bit = _ctz64(mask);
					mask &= ~(1ull << bit);
					const size_t index = i * 64 + bit;
					client_t* client = &clients[index];

					//printf("client %zu update\n", index);

					if (client->state == CLIENT_READ_HTTP_REQUEST)
					{
						char buf[4096];
						size_t nread;
						r = sw_recv(client->s, buf, sizeof(buf), &nread);
						if (r == SW_WOULD_BLOCK) continue;
						if (r != SW_OK)
						{
							printf("listener %zu disconnected\n", index);
							client_alloc_mask[i] &= ~(1ull << bit);
							continue;
						}

						printf("listener %zu HTTP nread = %zu\n", index, nread);

						const llhttp_errno_t err = llhttp_execute(&client->http_parser, buf, nread);
						assert(err == HPE_OK);
					}
					//else if (client->state == CLIENT_AWAIT_STREAMER)
					//{
					//	if (streamer_state == STREAMER_STREAM)
					//	{
					//		client->state = CLIENT_SEND_HTTP_HEADERS;
					//	}
					//}
					else if (client->state == CLIENT_SEND_HTTP_HEADERS)
					{
						const size_t http_remaining = g_http_response_len - client->http_tail;

						printf("listener %zu http_remaining = %zu\n", index, http_remaining);

						size_t nsent;
						r = sw_send(client->s, g_http_response + client->http_tail, http_remaining, &nsent);
						if (r == SW_WOULD_BLOCK) continue;
						if (r != SW_OK)
						{
							printf("listener %zu disconnected\n", index);
							client_alloc_mask[i] &= ~(1ull << bit);
							continue;
						}

						client->http_tail += nsent;
						if (client->http_tail == g_http_response_len)
						{
							client->state = CLIENT_SEND_OGG_HEADERS;
						}
					}
					else if (client->state == CLIENT_SEND_OGG_HEADERS)
					{
						assert(client->ogg_headers_cursor < ogg_header_buf_pos);

						const size_t ogg_remaining = ogg_header_buf_pos - client->ogg_headers_cursor;

						size_t nsent;
						r = sw_send(client->s, ogg_header_buf + client->ogg_headers_cursor, ogg_remaining, &nsent);
						if (r == SW_WOULD_BLOCK) continue;
						if (r != SW_OK)
						{
							printf("listener %zu disconnected\n", index);
							client_alloc_mask[i] &= ~(1ull << bit);
							continue;
						}
						//assert(r == SW_OK);

						client->ogg_headers_cursor += nsent;
						if (client->ogg_headers_cursor == ogg_header_buf_pos)
						{
							printf("OGG headers sent to listener\n");
							client->state = CLIENT_SEND_HTTP_CHUNK_HEADER;
						}
					}
					else if (client->state == CLIENT_SEND_HTTP_CHUNK_HEADER)
					{
						if (client->http_chunk_tail == http_chunk_head)
						{
							continue;
						}

						printf("http_chunk_tail = %zu\n", client->http_chunk_tail);

						assert(client->http_chunk_tail < http_chunk_head);

						const http_chunk_t* chunk = &http_chunks[client->http_chunk_tail];

						char chunklen[16];
						int n = sprintf(chunklen, "%zX\r\n", chunk->size);

						size_t nsent;
						r = sw_send(client->s, chunklen, n, &nsent);
						if (r == SW_WOULD_BLOCK) continue;
						if (r != SW_OK)
						{
							printf("listener %zu disconnected\n", index);
							client_alloc_mask[i] &= ~(1ull << bit);
							continue;
						}

						assert(nsent == n);

						client->state = CLIENT_SEND_HTTP_CHUNK_BODY;
					}
					else if (client->state == CLIENT_SEND_HTTP_CHUNK_BODY)
					{
						assert(client->http_chunk_tail != http_chunk_head);

						const http_chunk_t* chunk = &http_chunks[client->http_chunk_tail];
						assert(client->http_chunk_cursor < chunk->size);

						const size_t http_remaining = chunk->size - client->http_chunk_cursor;

						printf("http_remaining = %zu\n", http_remaining);

						size_t nsent;
						r = sw_send(client->s, chunk->data + client->http_chunk_cursor, http_remaining, &nsent);
						if (r == SW_WOULD_BLOCK) continue;
						if (r == SW_ERR || r == SW_CLOSED)
						{
							printf("listener %zu disconnected\n", index);
							client_alloc_mask[i] &= ~(1ull << bit);
							continue;
						}
						assert(r == SW_OK);

						printf("listener %zu nsent = %zu\n", index, nsent);

						client->http_chunk_cursor += nsent;
						if (client->http_chunk_cursor == chunk->size)
						{
							char buf[4];
							strcpy(buf, "\r\n");
							r = sw_send(client->s, buf, 2, &nsent);
							assert(r == SW_OK);
							assert(nsent == 2);

							client->http_chunk_cursor = 0;
							++client->http_chunk_tail;
							client->state = CLIENT_SEND_HTTP_CHUNK_HEADER;
						}
					}
				}
			}
		}
	}

	return 0;
}