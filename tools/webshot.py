# usage: tools/webshot.py URL OUT WAIT_S — headless Chrome via CDP, real-time wait (the SSE page never
# goes idle, so --screenshot/--virtual-time-budget fire too early or hang).
import asyncio, base64, json, os, signal, subprocess, sys, tempfile, time, urllib.request, websockets
url, out, wait = sys.argv[1], sys.argv[2], float(sys.argv[3])
prof = tempfile.TemporaryDirectory(ignore_cleanup_errors=True)
p = subprocess.Popen(["google-chrome", "--headless=new", "--disable-gpu", "--hide-scrollbars",
                      "--remote-debugging-port=9333", "--window-size=1280,3100", "--user-data-dir=" + prof.name, url],
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
try:
    for _ in range(50):
        try:
            tabs = json.load(urllib.request.urlopen("http://127.0.0.1:9333/json")); break
        except Exception: time.sleep(0.2)
    ws_url = [t for t in tabs if t["type"] == "page"][0]["webSocketDebuggerUrl"]
    async def run():
        async with websockets.connect(ws_url, max_size=None) as ws:
            await asyncio.sleep(wait)
            await ws.send(json.dumps({"id": 1, "method": "Page.captureScreenshot", "params": {"format": "png"}}))
            while True:
                m = json.loads(await ws.recv())
                if m.get("id") == 1:
                    open(out, "wb").write(base64.b64decode(m["result"]["data"])); return
    asyncio.run(run())
finally:
    os.killpg(p.pid, signal.SIGTERM)  # helpers too, before the profile is removed
    p.wait()
