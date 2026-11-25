"""Deterministic fictional fixtures and compatible Neutrino/SillyTavern exports."""
import base64
import json
import argparse
import struct
import zlib
from pathlib import Path

FACT = "The emergency locker code is LANTERN QUARTZ SEVEN."
GREETING = "The lantern flickers on the relay desk."
PROMPT = "What is the emergency locker code? Reply with the code only."
CARD = {"name":"Relay Bench","description":"A keeper of the coastal relay.","first_mes":GREETING}
HISTORY_SIZES = (10,100,500,1000,10000)

def png_card(card):
    def chunk(kind, payload):
        body = kind + payload
        return struct.pack(">I", len(payload)) + body + struct.pack(">I", zlib.crc32(body))

    header = struct.pack(">IIBBBBB", 1, 1, 8, 6, 0, 0, 0)
    pixel = zlib.compress(b"\0\x70\x90\xa0\xff")
    metadata = b"chara\0" + base64.b64encode(json.dumps(card).encode())
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) + chunk(b"IDAT", pixel) + chunk(b"tEXt", metadata) + chunk(b"IEND", b"")

def build(path, turns):
    path.mkdir(parents=True,exist_ok=True)
    pairs=[("I record the emergency locker code.",FACT)]
    pairs.extend((f"Log routine watch number {index}.",
                  f"Watch {index} passed. The dock stayed quiet, the lamp burned, and the log was closed.")
                 for index in range(1,turns))
    messages=[("assistant",GREETING)]
    for player, assistant in pairs: messages.extend((("user",player),("assistant",assistant)))
    nodes=[{"id":i+1,"parent":i,"role":role,"content":text} for i,(role,text) in enumerate(messages)]
    story={"id":"relay","title":"Night at the Relay","card":CARD,"head":len(nodes),"nodes":nodes,
           "memoryEnabled":False,"simEnabled":True}
    workspace={"records":[],"config":{},"stories":[story],"contextBudget":4096,
               "neutrinoSelection":{"roleplay":True,"story":"relay"}}
    (path/"workspace.json").write_text(json.dumps(workspace),encoding="utf-8")
    user=path/"st/default-user"
    (user/"characters").mkdir(parents=True,exist_ok=True)
    chat=user/"chats/RelayBench"
    chat.mkdir(parents=True,exist_ok=True)
    card={"spec":"chara_card_v2","spec_version":"2.0","chat":"relay-bench","data":{**CARD,"chat":"relay-bench",
          "personality":"","scenario":"","mes_example":"","creator_notes":"","tags":[],
          "alternate_greetings":[],"system_prompt":"","post_history_instructions":"","extensions":{}}}
    (user/"characters/RelayBench.png").write_bytes(png_card(card))
    journal=[{"user_name":"Player","character_name":"Relay Bench","create_date":"2026-01-01","chat_metadata":{}}]
    journal.extend({"name":"Player" if role=="user" else "Relay Bench","is_user":role=="user",
                    "is_system":False,"send_date":"2026-01-01T00:00:00.000Z","mes":text,"extra":{}}
                   for role,text in messages)
    (chat/"relay-bench.jsonl").write_text("\n".join(map(json.dumps,journal))+"\n",encoding="utf-8")

if __name__ == "__main__":
    parser=argparse.ArgumentParser()
    parser.add_argument("folder",type=Path)
    parser.add_argument("turns",type=int)
    args=parser.parse_args()
    build(args.folder,args.turns)
