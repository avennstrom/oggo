#define SW_SOCKET_IMPLEMENTATION
#include "socket.h"
#include "llhttp/llhttp.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#define MAX_CLIENTS (1024)
#define PASSWORD "125b69e01a6ecb38220b2fd425201f08e6950f09e6daaaf914b26718b88d09ab"
#define BUFFER_SIZE (64 * 1024 * 1024)
#define OGG_HEADER_BUF_SIZE (64 * 1024)

#define LISTENER_PORT (30000)
#define STREAMER_PORT (30001)

#define HTTP_CHUNK_COUNT (1024)

typedef struct {
	char password[64];
	uint32_t ogg_headers_size;
} stream_handshake_t;

static const char HTTP_RESPONSE_503_SERVICE_UNAVAILABLE[] = 
	"HTTP/1.1 503 SERVICE UNAVAILABLE\r\n"
	"Server: oggo\r\n"
	"Retry-After: 60\r\n"
	"Connection: close\r\n"
	"\r\n";

static const char HTTP_RESPONSE_200_OK[] = 
	"HTTP/1.1 200 OK\r\n"
	"Server: oggo\r\n"
	"Content-Type: audio/ogg\r\n"
	"Transfer-Encoding: chunked\r\n"
	"Connection: keep-alive\r\n"
	"Cache-Control: no-cache\r\n"
	"\r\n";
//static size_t g_http_response_len = sizeof(HTTP_RESPONSE_200_OK) - 1;

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
	uint8_t is_service_unavailable : 1;
	sw_socket_t s;
	uint8_t http_cursor;
	size_t http_chunk_tail;
	size_t http_chunk_cursor;
	size_t ogg_headers_cursor;

	// http parsing state
	llhttp_t http_parser;
	size_t header_len;
	size_t header_val_len;
	char header[256];
	char header_val[256];
	char user_agent[256];
} client_t;

static client_t clients[MAX_CLIENTS];
static uint64_t client_alloc_mask[MAX_CLIENTS / 64];

static inline size_t client__index(client_t* client)
{
	return client - clients;
}

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

static int client__llhttp_on_message_begin(llhttp_t* parser)
{
	client_t* client = parser->data;
	client->header_len = 0;
	client->header_val_len = 0;
	return 0;
}

static int client__llhttp_on_header_field(llhttp_t* parser, const char* at, size_t len)
{
	client_t* client = parser->data;

	const size_t remaining = (sizeof(client->header) - 1) - client->header_len;
	if (len > remaining) len = remaining;
	if (len == 0) return 0;

	memcpy(client->header + client->header_len, at, len);
	client->header_len += len;
	return 0;
}

static int client__llhttp_on_header_field_complete(llhttp_t* parser)
{
	client_t* client = parser->data;
	assert(client->header_len < sizeof(client->header));
	client->header[client->header_len] = '\0';
	return 0;
}


static int client__llhttp_on_header_value(llhttp_t* parser, const char* at, size_t len)
{
	client_t* client = parser->data;

	const size_t remaining = (sizeof(client->header_val) - 1) - client->header_val_len;
	if (len > remaining) len = remaining;
	if (len == 0) return 0;

	memcpy(client->header_val + client->header_val_len, at, len);
	client->header_val_len += len;
	return 0;
}

static int client__llhttp_on_header_value_complete(llhttp_t* parser)
{
	client_t* client = parser->data;
	assert(client->header_val_len < sizeof(client->header_val));
	client->header_val[client->header_val_len] = '\0';

	printf("c[%zu] [HTTP] %s: %s\n", client__index(client), client->header, client->header_val);

	// interesting headers?
	if (_stricmp(client->header, "user-agent"))
	{
		strcpy(client->user_agent, client->header_val);
	}

	client->header_len = 0;
	client->header_val_len = 0;
	return 0;
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

	llhttp_settings_init(&llhttp_settings);
	llhttp_settings.on_message_begin = client__llhttp_on_message_begin;
	llhttp_settings.on_header_field = client__llhttp_on_header_field;
	llhttp_settings.on_header_field_complete = client__llhttp_on_header_field_complete;
	llhttp_settings.on_header_value = client__llhttp_on_header_value;
	llhttp_settings.on_header_value_complete = client__llhttp_on_header_value_complete;
	llhttp_settings.on_message_complete = client__llhttp_on_message_complete;

	sw_init();

	sw_socket_t broadcast_socket = sw_socket_create();
	sw_socket_t streamer_server_socket = sw_socket_create();

	sw_socket_set_nonblocking(broadcast_socket, 1);
	sw_socket_set_nonblocking(streamer_server_socket, 1);

	sw_bind(broadcast_socket, LISTENER_PORT);
	sw_listen(broadcast_socket, 64);
	printf("Listening for listeners on port %d\n", LISTENER_PORT);
	sw_bind(streamer_server_socket, STREAMER_PORT);
	sw_listen(streamer_server_socket, 1);
	printf("Listening for streamer on port %d\n", STREAMER_PORT);

	struct streamer {
		enum {
			STREAMER_HANDSHAKE = 0,
			STREAMER_OGG_HEADERS,
			STREAMER_STREAM,
		} state;
		sw_socket_t s;
		stream_handshake_t handshake;
	} streamer = {
		.s = SW_INVALID_SOCKET,
	};

	for (;;)
	{
		sw_socket_t accepted_socket;
		r = sw_accept(broadcast_socket, &accepted_socket);
		if (r == SW_OK)
		{
			const size_t client_index = client_alloc();
			printf("c[%zu] connected!\n", client_index);
			sw_socket_set_nonblocking(accepted_socket, 1);

			client_t* client = &clients[client_index];
			client->s = accepted_socket;
			client->state = CLIENT_READ_HTTP_REQUEST;
			client->http_cursor = 0;
			client->http_chunk_cursor = 0;
			client->http_chunk_tail = http_chunk_head;
			client->ogg_headers_cursor = 0;
			llhttp_init(&client->http_parser, HTTP_REQUEST, &llhttp_settings);
			client->http_parser.data = client;
			//llhttp_reset(&client->http_parser);
		}

		// only accept a single streamer!
		if (streamer.s == SW_INVALID_SOCKET)
		{
			// disconnect all listeners
			// :TODO: maybe we want to send empty frames or elevator music when there is not streamer live
			//for (size_t i = 0; i < MAX_CLIENTS / 64; ++i)
			//{
			//	uint64_t mask = client_alloc_mask[i];
			//	while (mask)
			//	{
			//		const size_t bit = _ctz64(mask);
			//		mask &= ~(1ull << bit);
			//		const size_t index = i * 64 + bit;
			//		client_t* client = &clients[index];

			//		sw_socket_close(client->s);
			//		client->s = SW_INVALID_SOCKET;

			//		printf("listener %zu closed\n", index);
			//	}

			//	client_alloc_mask[i] = 0;
			//}

			r = sw_accept(streamer_server_socket, &streamer.s);
			if (r == SW_OK)
			{
				printf("streamer connected\n");
				sw_socket_set_nonblocking(streamer.s, 1);
				ogg_header_buf_pos = 0;
				//http_chunk_head = 0;
				streamer.state = STREAMER_HANDSHAKE;
			}
		}

		if (streamer.s != SW_INVALID_SOCKET)
		{
			switch (streamer.state)
			{
				case STREAMER_HANDSHAKE:
				{
					size_t nread;
					r = sw_recv(streamer.s, &streamer.handshake, sizeof(streamer.handshake), &nread);
					if (r == SW_WOULD_BLOCK) break;
					if (r != SW_OK)
					{
						printf("streamer disconnected!\n");
						streamer.s = SW_INVALID_SOCKET;
						break;
					}
					assert(nread == sizeof(streamer.handshake));

					if (memcmp(streamer.handshake.password, PASSWORD, sizeof(streamer.handshake.password)) != 0)
					{
						sw_socket_close(streamer.s);
						break;
					}

					printf("<<<< streamer handshake (ogg_headers_size = %u)\n", streamer.handshake.ogg_headers_size);
					hexdump(&streamer.handshake, sizeof(streamer.handshake));

					ogg_header_hexlen_size = sprintf(ogg_header_buf + ogg_header_buf_pos, "%X\r\n", streamer.handshake.ogg_headers_size);
					ogg_header_buf_pos += ogg_header_hexlen_size;

					streamer.state = STREAMER_OGG_HEADERS;
					break;
				}
				case STREAMER_OGG_HEADERS:
				{
					size_t nread;
					r = sw_recv(streamer.s, ogg_header_buf + ogg_header_buf_pos, ogg_header_hexlen_size + streamer.handshake.ogg_headers_size - ogg_header_buf_pos, &nread);
					if (r == SW_WOULD_BLOCK) break;
					ogg_header_buf_pos += nread;
					if (ogg_header_buf_pos == ogg_header_hexlen_size + streamer.handshake.ogg_headers_size)
					{
						ogg_header_buf_pos += sprintf(ogg_header_buf + ogg_header_buf_pos, "\r\n", 2);
						streamer.state = STREAMER_STREAM;
						break;
					}
					break;
				}
				case STREAMER_STREAM:
				{
					size_t nread;
					r = sw_recv(streamer.s, ogg_buf + ogg_buf_head, 64 * 1024, &nread);
					if (r == SW_WOULD_BLOCK) break;
					if (r != SW_OK)
					{
						printf("streamer disconnected!\n");
						streamer.s = SW_INVALID_SOCKET;
						break;
					}
					assert(nread > 0);

					//printf("nread = %zu\n", nread);

					http_chunks[http_chunk_head % HTTP_CHUNK_COUNT] = (http_chunk_t){
						.size = nread,
						.data = ogg_buf + ogg_buf_head,
					};
					++http_chunk_head;

					ogg_buf_head += nread;
				}
				break;
			}

		}

		size_t min_http_chunk_tail = http_chunk_head;

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

				if (0)
				{
				_disconnect:
					printf("c[%zu] disconnected\n", index);
					client_alloc_mask[i] &= ~(1ull << bit);
					sw_socket_close(client->s);
					client->s = SW_INVALID_SOCKET;
					continue;
				}

				if (min_http_chunk_tail > client->http_chunk_tail)
				{
					min_http_chunk_tail = client->http_chunk_tail;
				}

				if (client->state == CLIENT_READ_HTTP_REQUEST)
				{
					char buf[4096];
					size_t nread;
					r = sw_recv(client->s, buf, sizeof(buf), &nread);
					if (r == SW_WOULD_BLOCK) continue;
					if (r != SW_OK)
					{
						goto _disconnect;
					}

					//printf("<<<< listener [%zu] HTTP nread = %zu\n", index, nread);
					//hexdump(buf, nread);

					client->is_service_unavailable = streamer.s == SW_INVALID_SOCKET;

					// the client->state is mutated inside the llhttp parsing callback (client__llhttp_on_message_complete)
					const llhttp_errno_t err = llhttp_execute(&client->http_parser, buf, nread);
					if (err != HPE_OK)
					{
						goto _disconnect;
					}
				}
				else if (client->state == CLIENT_SEND_HTTP_HEADERS)
				{
					const char* http_response = client->is_service_unavailable ? 
						HTTP_RESPONSE_503_SERVICE_UNAVAILABLE : 
						HTTP_RESPONSE_200_OK;
					const size_t http_response_len = strlen(http_response);

					const size_t http_remaining = http_response_len - client->http_cursor;

					printf(">>>>\n");
					hexdump(http_response + client->http_cursor, http_remaining);

					size_t nsent;
					r = sw_send(client->s, http_response + client->http_cursor, http_remaining, &nsent);
					if (r == SW_WOULD_BLOCK) continue;
					if (r != SW_OK)
					{
						goto _disconnect;
					}

					client->http_cursor += nsent;
					if (client->http_cursor == http_response_len)
					{
						if (client->is_service_unavailable)
						{
							goto _disconnect;
						}
						else
						{
							client->state = CLIENT_SEND_OGG_HEADERS;
						}
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
						goto _disconnect;
					}
					//assert(r == SW_OK);

					client->ogg_headers_cursor += nsent;
					if (client->ogg_headers_cursor == ogg_header_buf_pos)
					{
						printf("c[%zu] OGG headers sent (%zu bytes)\n", index, ogg_header_buf_pos);
						client->state = CLIENT_SEND_HTTP_CHUNK_HEADER;
					}
				}
				else if (client->state == CLIENT_SEND_HTTP_CHUNK_HEADER)
				{
					if (client->http_chunk_tail == http_chunk_head)
					{
						continue;
					}

					//printf("http_chunk_tail = %zu\n", client->http_chunk_tail);

					assert(client->http_chunk_tail < http_chunk_head);

					const http_chunk_t* chunk = &http_chunks[client->http_chunk_tail % HTTP_CHUNK_COUNT];

					char chunklen[16];
					int n = sprintf(chunklen, "%zX\r\n", chunk->size);

					size_t nsent;
					r = sw_send(client->s, chunklen, n, &nsent);
					if (r == SW_WOULD_BLOCK) continue;
					if (r != SW_OK)
					{
						goto _disconnect;
					}

					assert(nsent == n);

					client->state = CLIENT_SEND_HTTP_CHUNK_BODY;
				}
				else if (client->state == CLIENT_SEND_HTTP_CHUNK_BODY)
				{
					assert(client->http_chunk_tail != http_chunk_head);

					const http_chunk_t* chunk = &http_chunks[client->http_chunk_tail % HTTP_CHUNK_COUNT];
					assert(client->http_chunk_cursor < chunk->size);

					const size_t http_remaining = chunk->size - client->http_chunk_cursor;

					//printf("http_remaining = %zu\n", http_remaining);

					size_t nsent;
					r = sw_send(client->s, chunk->data + client->http_chunk_cursor, http_remaining, &nsent);
					if (r == SW_WOULD_BLOCK) continue;
					if (r == SW_ERR || r == SW_CLOSED)
					{
						goto _disconnect;
					}
					assert(r == SW_OK);

					//printf("listener %zu nsent = %zu\n", index, nsent);

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

	return 0;
}