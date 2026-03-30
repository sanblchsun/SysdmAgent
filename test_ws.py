#!/usr/bin/env python3
import asyncio
import websockets
import json

async def test():
    uri = "ws://192.168.88.127:8000/ws/agent/test"
    print(f"Connecting to {uri}...")
    
    async with websockets.connect(uri) as ws:
        print("Connected!")
        
        # Send 10 messages
        for i in range(10):
            msg = json.dumps({"msg": i})
            await ws.send(msg)
            print(f"Sent #{i}: {msg}")
            
            # Try to receive
            try:
                response = await asyncio.wait_for(ws.recv(), timeout=1.0)
                print(f"Received: {response}")
            except asyncio.TimeoutError:
                print(f"No response for #{i}")
        
        print("Done!")

asyncio.run(test())
