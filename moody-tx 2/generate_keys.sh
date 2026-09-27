#!/usr/bin/env bash
set -euo pipefail

project_dir="$(cd "$(dirname "$0")" && pwd)"
provision_dir="$project_dir/provisioning"
mkdir -p "$provision_dir"

openssl ecparam -name prime256v1 -genkey -noout \
  -out "$provision_dir/moody-tx-private.pem"
openssl ec -in "$provision_dir/moody-tx-private.pem" -pubout \
  -out "$provision_dir/moody-tx-public.pem"

openssl ecparam -name prime256v1 -genkey -noout \
  -out "$provision_dir/moody-rx-private.pem"
openssl ec -in "$provision_dir/moody-rx-private.pem" -pubout \
  -out "$provision_dir/moody-rx-public.pem"

{
  printf '%s\n' '#pragma once' ''
  printf '%s\n' 'static const char MOODY_TX_PRIVATE_KEY_PEM[] = R"PEM('
  sed -n 'p' "$provision_dir/moody-tx-private.pem"
  printf '%s\n' ')PEM";' ''
  printf '%s\n' 'static const char MOODY_RX_PUBLIC_KEY_PEM[] = R"PEM('
  sed -n 'p' "$provision_dir/moody-rx-public.pem"
  printf '%s\n' ')PEM";'
} > "$project_dir/moody_keys.h"

chmod 600 "$provision_dir"/*-private.pem

printf '%s\n' 'Generated permanent moody-tx/moody-rx P-256 identities.'
printf '%s\n' 'Keep provisioning/*-private.pem secret and backed up.'
