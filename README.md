# oggo

Oggo is a tiny OGG stream multiplexer packaged in two parts:

* Client that transmits a stream of OGG packets to the relay
    * The default audio capture device of the OS is used as audio source

* Relay that receives OGG packets from the client and broadcasts it to connected listeners over HTTP

```
 |--------|                        |-------|
 | client | --(HANDSHAKE + OGG)--> | relay |
 |--------|                        |-------|
                                       |
                                       |
                         (HTTP Content-Type: audio/ogg)
                                       |
                       |---------------|---------------|
                       |               |               |
                  |----------|    |----------|    |----------|
                  | listener |    | listener |    | listener |
                  |----------|    |----------|    |----------|
```

Visit https://vennstrom.dev/stream to listen to an oggo stream right now!
It is live audio from my modular synthesizer at home.

## Dependencies

* libogg 1.3.6 (external/ogg)
* libvorbis 1.3.7 (external/vorbis)

## Building

I use cmake
