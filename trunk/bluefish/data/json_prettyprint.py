#!/usr/bin/python

import json
import sys

buf = sys.stdin.read()
#json.load method converts JSON string to Python Object
parsed = json.loads(buf)
sys.stdout.write(json.dumps(parsed, indent=2, sort_keys=True))
