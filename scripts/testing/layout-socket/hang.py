"""Accepts TCP and never answers the HTTP upgrade: the case a connect-phase bound must survive."""
import socket, time
srv = socket.socket(); srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind(("127.0.0.1", 18766)); srv.listen(1)
conn, _ = srv.accept(); time.sleep(60)
