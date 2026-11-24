#!/usr/bin/env bash
#
# analytics.sh — print a GoatCounter usage summary for the hosted configurator.
#
# Reads GOATCOUNTER_TOKEN from the repo-root .env (gitignored; never commit it).
# The token is only ever sent to GoatCounter's own API. Nothing is stored.
#
# Usage:
#   scripts/analytics.sh            # last 14 days
#   scripts/analytics.sh 30         # last 30 days
#   scripts/analytics.sh 2026-11-01 2026-11-24   # explicit start/end
#
# Dashboard: https://headunit.goatcounter.com   (login-free if made public)

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
[ -f "$ROOT/.env" ] || { echo "No .env at $ROOT/.env (needs GOATCOUNTER_TOKEN=...)"; exit 1; }
set -a; . "$ROOT/.env"; set +a
: "${GOATCOUNTER_TOKEN:?GOATCOUNTER_TOKEN not set in .env}"

command -v jq >/dev/null || { echo "jq is required (brew install jq)"; exit 1; }

# --- date range -------------------------------------------------------------
if [[ "${1:-}" =~ ^[0-9]{4}-[0-9]{2}-[0-9]{2}$ ]]; then
  START="$1"; END="${2:-$(date +%F)}"
else
  DAYS="${1:-14}"
  START="$(date -v-"${DAYS}"d +%F)"; END="$(date +%F)"
fi

BASE="https://headunit.goatcounter.com/api/v0"
AUTH="Authorization: Bearer ${GOATCOUNTER_TOKEN}"
get(){ curl -s -H "$AUTH" "$BASE/$1?start=$START&end=$END${2:-}"; }

echo "GoatCounter — headunit-toolkit   ($START → $END)"
echo "──────────────────────────────────────────────"

# --- totals -----------------------------------------------------------------
tot="$(get stats/total)"
views="$(jq -r '.total // 0' <<<"$tot")"
events="$(jq -r '.total_events // 0' <<<"$tot")"
printf "Page views : %s\n" "$views"
printf "Build events: %s   (people who actually built an image)\n" "$events"

# --- pages ------------------------------------------------------------------
echo
echo "Top pages / events"
hits="$(get stats/hits '&limit=20')"
if [ "$(jq -r '.hits | length' <<<"$hits")" -gt 0 ]; then
  jq -r '.hits[] | "  \(.count)\t\(.path)\(if .event then "  [event]" else "" end)"' <<<"$hits"
else
  echo "  (none yet)"
fi

# --- referrers --------------------------------------------------------------
echo
echo "Top referrers   (which outreach drove traffic)"
refs="$(get stats/toprefs)"
if [ "$(jq -r '.stats | length' <<<"$refs")" -gt 0 ]; then
  jq -r '.stats[] | "  \(.count)\t\(.name // "(direct / none)")"' <<<"$refs"
else
  echo "  (none yet)"
fi
echo
