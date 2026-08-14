#!/usr/bin/env bash
set -euo pipefail

SCRIPT_PATH=$(dirname "$(readlink -f "$0")")
PROJECT_ROOT=$(readlink -f "${SCRIPT_PATH}/../..")
echo "USER:$USER"
echo "SCRIPT_PATH:${SCRIPT_PATH}"
if [ "$EUID" -ne 0 ]; then
  echo -e "\e[31mPlease run this script as root\e[0m"
  exit 1
fi
umask 022
PM_TINY_CFG_DIR="/usr/local/pm_tiny"
PM_TINY_CFG_FILE="${PM_TINY_CFG_DIR}/pm_tiny.yaml"
PM_TINY_PROG_CFG_FILE="${PM_TINY_CFG_DIR}/prog.yaml"
mkdir -p "${PM_TINY_CFG_DIR}"
mkdir -p /var/log/pm_tiny

if [ ! -f "${PM_TINY_CFG_FILE}" ]; then
  cp "${PROJECT_ROOT}/examples/config/linux/pm_tiny.yaml" "${PM_TINY_CFG_FILE}"
fi
if [ ! -f "${PM_TINY_PROG_CFG_FILE}" ]; then
  cp "${PROJECT_ROOT}/examples/config/linux/prog.yaml" "${PM_TINY_PROG_CFG_FILE}"
fi
cp -f "${PROJECT_ROOT}/packaging/systemd/pm_tiny.service" /lib/systemd/system/
systemctl daemon-reload
systemctl enable pm_tiny.service
echo "========================="
echo -e "Start pm_tiny with the following command:\n"
echo -e "systemctl start pm_tiny"
echo "========================="
