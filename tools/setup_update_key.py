"""One-off setup for the optional self-update from Home Assistant (firmware/main/selfupdate.c).

Generates a random key and stores it, without printing it, in firmware/main/secrets.h
(git-ignored) together with your Home Assistant addresses:

    python tools/setup_update_key.py --url https://xxxx.ui.nabu.casa --local-url http://192.168.0.10:8123

--url is how the display reaches HA from anywhere (Nabu Casa or your own HTTPS address);
--local-url (optional) is tried first on networks marked .home in secrets.h.

The same key must go in HA's /config/secrets.yaml as `van_display_key`. With --ssh HOST
(e.g. --ssh root@homeassistant.local, needs the SSH add-on) it is appended there for you, after a
timestamped backup. Without it, the line to paste is written to tools/van_display_key.yaml
(git-ignored); delete that file once it is in HA. See homeassistant/README.md.
"""
import argparse
from datetime import datetime
from pathlib import Path
import secrets
import subprocess
import sys

ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
ap.add_argument("--url", required=True, help="HA address reachable from anywhere, e.g. https://xxxx.ui.nabu.casa")
ap.add_argument("--local-url", help="HA address on the home network, e.g. http://192.168.0.10:8123")
ap.add_argument("--ssh", help="ssh target for HA, to add the key to its secrets.yaml")
args = ap.parse_args()

root = Path(__file__).resolve().parent.parent
secrets_h = root / "firmware" / "main" / "secrets.h"
if not secrets_h.exists():
    sys.exit("No firmware/main/secrets.h yet: copy secrets.example.h to secrets.h and fill it in first")
if "#define DISPLAY_UPDATE_KEY" in secrets_h.read_text(encoding="utf-8"):
    sys.exit("secrets.h already has DISPLAY_UPDATE_KEY")

key = secrets.token_hex(32)
entry = f"\n# Van display self-update (van_display_update), also in the display's secrets.h\nvan_display_key: {key}\n"
if args.ssh:
    if subprocess.run(["ssh", args.ssh, "! grep -q '^van_display_key:' /config/secrets.yaml"]).returncode:
        sys.exit("HA's secrets.yaml already has van_display_key")
    stamp = datetime.now().strftime("%Y%m%d-%H%M")
    if subprocess.run(["ssh", args.ssh, f"cp /config/secrets.yaml /config/secrets.yaml.bak-{stamp}"]).returncode:
        sys.exit("backing up secrets.yaml failed")
    if subprocess.run(["ssh", args.ssh, "cat >> /config/secrets.yaml"], input=entry.encode()).returncode:
        sys.exit("adding the key to secrets.yaml failed")
    where = f"HA's secrets.yaml (backup secrets.yaml.bak-{stamp})"
else:
    snippet = root / "tools" / "van_display_key.yaml"
    snippet.write_text(entry.lstrip(), encoding="utf-8")
    where = f"{snippet.relative_to(root)} (paste it into HA's secrets.yaml, then delete the file)"

with open(secrets_h, "a", encoding="utf-8", newline="\n") as f:
    f.write("\n/* Self-update from Home Assistant (selfupdate.c); key = van_display_key in HA's secrets.yaml */\n")
    f.write(f'#define DISPLAY_HA_URL "{args.url.rstrip("/")}"\n')
    if args.local_url:
        f.write(f'#define DISPLAY_HA_LOCAL_URL "{args.local_url.rstrip("/")}"\n')
    f.write(f'#define DISPLAY_UPDATE_KEY "{key}"\n')
print(f"Key stored in secrets.h and {where}.")
