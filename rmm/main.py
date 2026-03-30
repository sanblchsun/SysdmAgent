# rmm/main.py
from fastapi import FastAPI, WebSocket, WebSocketDisconnect, Request, HTTPException
from fastapi.middleware.cors import CORSMiddleware
from fastapi.templating import Jinja2Templates
from starlette.responses import Response
import os
import asyncio
import struct
import logging
from typing import Dict

logging.basicConfig(
    level=logging.DEBUG,
    format="%(asctime)s [%(levelname)s] %(message)s",
)
logger = logging.getLogger("rmm")

app = FastAPI(title="RMM Signaling Server")

app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_credentials=True,
    allow_methods=["*"],
    allow_headers=["*"],
)

BASE_DIR = os.path.dirname(os.path.abspath(__file__))
templates = Jinja2Templates(directory=os.path.join(BASE_DIR, "templates"))

agents: Dict[str, WebSocket] = {}
viewers: Dict[str, WebSocket] = {}
last_frame: Dict[str, bytes] = {}


@app.get("/")
async def index(request: Request):
    return templates.TemplateResponse("viewer.html", {"request": request})


@app.get("/status")
async def status():
    return {
        "agents": list(agents.keys()),
        "viewers": list(viewers.keys()),
        "has_frame": {k: len(v) > 0 for k, v in last_frame.items()}
    }


@app.post("/frame/{agent_id}")
async def receive_frame(agent_id: str, request: Request):
    body = await request.body()
    logger.info(f"HTTP Frame from {agent_id}: {len(body)} bytes")
    
    if len(body) < 16:
        return Response("Too small", status_code=400)
    
    width, height, timestamp = struct.unpack('<iid', body[:16])
    jpeg_data = body[16:]
    
    logger.info(f"Frame: {width}x{height}, JPEG={len(jpeg_data)} bytes")
    
    last_frame[agent_id] = body
    
    viewer = viewers.get(agent_id)
    if viewer:
        try:
            await viewer.send_bytes(body)
            logger.info(f"Forwarded frame to viewer")
        except Exception as e:
            logger.error(f"Error forwarding frame: {e}")
            viewers.pop(agent_id, None)
    
    return Response("OK", status_code=200)


@app.get("/frame/{agent_id}")
async def get_frame(agent_id: str):
    frame = last_frame.get(agent_id)
    if not frame:
        raise HTTPException(404, "No frame available")
    
    return Response(frame, media_type="application/octet-stream")


@app.websocket("/ws/agent/{agent_id}")
async def agent_ws(ws: WebSocket, agent_id: str):
    logger.info(f"=== Agent {agent_id} connecting...")
    await ws.accept()
    agents[agent_id] = ws
    logger.info(f"=== Agent {agent_id} CONNECTED")

    try:
        while True:
            try:
                data = await asyncio.wait_for(ws.receive(), timeout=5.0)
            except asyncio.TimeoutError:
                continue
                
            logger.info(f"Agent {agent_id}: type={data.get('type')}")

            if data["type"] == "websocket.disconnect":
                logger.info(f"Agent {agent_id}: disconnected")
                break

            if data["type"] == "websocket.receive":
                if "text" in data:
                    text_data = data["text"]
                    logger.info(f"Agent {agent_id}: TEXT [{len(text_data)} bytes]")
                    viewer = viewers.get(agent_id)
                    if viewer:
                        await viewer.send_text(text_data)
                        
                elif "bytes" in data:
                    bytes_data = data["bytes"]
                    logger.info(f"Agent {agent_id}: BYTES [{len(bytes_data)} bytes]")
                    viewer = viewers.get(agent_id)
                    if viewer:
                        await viewer.send_bytes(bytes_data)
                    else:
                        logger.warning(f"No viewer, dropped {len(bytes_data)} bytes")

    except WebSocketDisconnect:
        logger.info(f"Agent {agent_id}: WebSocketDisconnect")
    except Exception as e:
        logger.exception(f"Agent {agent_id}: exception: {e}")
    finally:
        agents.pop(agent_id, None)


@app.websocket("/ws/viewer/{agent_id}")
async def viewer_ws(ws: WebSocket, agent_id: str):
    logger.info(f"=== Viewer {agent_id} connecting...")
    await ws.accept()
    
    old = viewers.get(agent_id)
    if old:
        try:
            await old.close()
        except:
            pass

    viewers[agent_id] = ws
    logger.info(f"=== Viewer {agent_id} CONNECTED")

    try:
        while True:
            data = await ws.receive()
            
            if data["type"] == "websocket.disconnect":
                break

            if data["type"] == "websocket.receive":
                text_data = data.get("text")
                if text_data:
                    logger.info(f"Viewer {agent_id}: TEXT: {text_data[:50]}")
                    agent = agents.get(agent_id)
                    if agent:
                        await agent.send_text(text_data)

    except WebSocketDisconnect:
        pass
    except Exception as e:
        logger.exception(f"Viewer {agent_id}: exception: {e}")
    finally:
        viewers.pop(agent_id, None)


if __name__ == "__main__":
    import uvicorn
    uvicorn.run(app, host="0.0.0.0", port=8000)
