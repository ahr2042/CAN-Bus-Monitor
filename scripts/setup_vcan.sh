#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# setup_vcan.sh -- Create a virtual CAN interface for development/testing
#
# Copyright (C) 2026  Ahmad Rashed
#
# Usage:
#   sudo ./scripts/setup_vcan.sh [up|down] [interface]
#
# Defaults: action=up, interface=vcan0
#
# Prerequisites (Debian/Ubuntu):
#   sudo apt-get install can-utils
#   sudo modprobe vcan
#
set -euo pipefail

ACTION="${1:-up}"
IFACE="${2:-vcan0}"

case "$ACTION" in
  up)
    echo "Loading vcan kernel module..."
    modprobe vcan
    echo "Creating virtual CAN interface '$IFACE'..."
    ip link add dev "$IFACE" type vcan 2>/dev/null || true
    ip link set "$IFACE" up
    echo "Interface '$IFACE' is UP."
    echo ""
    echo "Test with:  cansend $IFACE 123#DEADBEEF"
    echo "Monitor with: candump $IFACE"
    ;;
  down)
    echo "Bringing down '$IFACE'..."
    ip link set "$IFACE" down 2>/dev/null || true
    ip link delete dev "$IFACE" 2>/dev/null || true
    echo "Done."
    ;;
  *)
    echo "Usage: $0 [up|down] [interface]" >&2
    exit 1
    ;;
esac
