#!/usr/bin/env bash
set -e
cd "$(dirname "$0")"
python -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt
cp -n .env.example .env || true
echo "✅ Backend ready. Run: source .venv/bin/activate && python -m app.main"
