"""Minimal stdlib websocket server that pings like uvicorn/websockets does.

Sends a PING carrying a 4-byte random payload every INTERVAL seconds and
reports whether the matching PONG arrives before the next ping. Sends one text
snapshot after the handshake, as the control API does. Exists only to reproduce
the plugin's layout-socket disconnect loop.
"""
import asyncio, os, ssl, sys, time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
from ws_server import accept, frame, read_frame  # noqa: E402

INTERVAL = float(sys.argv[1]) if len(sys.argv) > 1 else 3.0
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 18765
TLS_CERT = sys.argv[3] if len(sys.argv) > 3 else None  # cert.pem + key.pem alongside enables wss


def log(msg):
    print(f"{time.strftime('%H:%M:%S')} {msg}", flush=True)


async def handle(reader, writer):
    await accept(reader, writer)
    writer.write(frame(1, b'{"type":"layout.snapshot"}'))
    await writer.drain()
    log("client connected")
    pending = {}

    async def pinger():
        while True:
            await asyncio.sleep(INTERVAL)
            payload = os.urandom(4)
            pending[payload] = time.time()
            writer.write(frame(9, payload))
            await writer.drain()
            log(f"sent PING {payload.hex()}")

    task = asyncio.create_task(pinger())
    try:
        while True:
            opcode, data = await read_frame(reader)
            if opcode == 10:
                sent = pending.pop(data, None)
                log(f"got PONG {data.hex()} ({'matches' if sent else 'UNMATCHED'})")
            elif opcode == 8:
                log("got CLOSE"); break
            else:
                log(f"got opcode {opcode} len {len(data)}")
    except (asyncio.IncompleteReadError, ConnectionError) as error:
        log(f"client dropped the connection ({type(error).__name__})")
    finally:
        task.cancel()
        writer.close()


async def main():
    context = None
    if TLS_CERT:
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(TLS_CERT + "/cert.pem", TLS_CERT + "/key.pem")
    server = await asyncio.start_server(handle, "127.0.0.1", PORT, ssl=context)
    log(f"listening on {PORT}, ping every {INTERVAL}s")
    async with server:
        await server.serve_forever()

asyncio.run(main())
