#!/usr/bin/env node
// Test vectors for Ready Up's license key check (core/src/readyup/license.h), signed with a
// throwaway Ed25519 key pair made on every run: the private key is never written anywhere, and
// the real signing key is never needed. Output: tests/fixtures/license/vectors.json, read by
// tests/license_test.cpp (ctest `license`).
//
//   node tests/license/make_vectors.mjs > tests/fixtures/license/vectors.json
//   # also check every case against the website's reference verifier:
//   node tests/license/make_vectors.mjs --reference ../website/scripts/license-verify.mjs > ...
//
// Token format and signing are the website's (Auto-Tournament/website src/lib/license/format.ts).
import { createHash, generateKeyPairSync, sign } from 'node:crypto';
import { pathToFileURL } from 'node:url';
import path from 'node:path';

const PREFIX = 'ATL1';

function throwawayKey() {
  const { privateKey, publicKey } = generateKeyPairSync('ed25519');
  const x = publicKey.export({ format: 'jwk' }).x;
  const kid = createHash('sha256').update(Buffer.from(x, 'base64url')).digest('base64url').slice(0, 16);
  return { kid, x, privateKey };
}

function signLicense(payload, key) {
  const body = Buffer.from(JSON.stringify(payload), 'utf8').toString('base64url');
  const input = `${PREFIX}.${body}`;
  return `${input}.${sign(null, Buffer.from(input, 'ascii'), key.privateKey).toString('base64url')}`;
}

const key = throwawayKey();
const other = throwawayKey();

function payload(over = {}) {
  return {
    v: 1,
    kid: key.kid,
    id: 'L-test',
    customer: 'cus_TEST123',
    licensee: 'Example LAN AS',
    product: 'servers',
    pack: 'M',
    max_servers: 15,
    kind: 'year',
    issued_at: '2026-10-01T12:00:00Z',
    updates_until: '2027-10-01',
    ...over,
  };
}

const withoutLicensee = (p) => {
  const { licensee, ...rest } = p;
  return rest;
};

const year = signLicense(payload(), key);
const [, yearBody, yearSig] = year.split('.');
const event = signLicense(
  payload({ kind: 'event', pack: 'S', max_servers: 6, licensee: 'NTLAN', updates_until: '2026-10-16', valid_from: '2026-10-14', valid_to: '2026-10-16' }),
  key,
);

function flipSigByte(token) {
  const [p, b, s] = token.split('.');
  const sig = Buffer.from(s, 'base64url');
  sig[0] ^= 1;
  return `${p}.${b}.${sig.toString('base64url')}`;
}

// S + L: the same signature in a non-canonical form. Node (OpenSSL) and Monocypher both refuse it.
function nonCanonicalS(token) {
  const [p, b, s] = token.split('.');
  const sig = Buffer.from(s, 'base64url');
  const L = (1n << 252n) + 27742317777372353535851937790883648493n;
  let n = 0n;
  for (let i = 31; i >= 0; i--) n = (n << 8n) | BigInt(sig[32 + i]);
  n += L;
  for (let i = 0; i < 32; i++) {
    sig[32 + i] = Number(n & 0xffn);
    n >>= 8n;
  }
  return `${p}.${b}.${sig.toString('base64url')}`;
}

const tamperedBody = Buffer.from(JSON.stringify({ ...payload(), max_servers: 999 })).toString('base64url');
const v2 = Buffer.from(JSON.stringify({ ...payload(), v: 2 })).toString('base64url');

// expect: status (ok | warning | invalid) and the issue codes, in order.
const cases = [
  { name: 'year key, covered line, no warnings', token: year, line_date: '2027-03-01', today: '2027-03-01', status: 'ok', codes: [],
    console: 'License: Example LAN AS · Servers M (15 servers) · yearly, updates until 2027-10-01 · valid (license L-test)',
    player: 'Licensed to Example LAN AS' },
  { name: 'surrounding whitespace is ignored', token: `  ${year}\n`, line_date: '2027-03-01', today: '2027-03-01', status: 'ok', codes: [] },
  { name: 'tampered payload', token: `${PREFIX}.${tamperedBody}.${yearSig}`, line_date: '2027-03-01', today: '2027-03-01', status: 'invalid', codes: ['bad_signature'], player: '' },
  { name: 'flipped signature byte', token: flipSigByte(year), line_date: '2027-03-01', today: '2027-03-01', status: 'invalid', codes: ['bad_signature'] },
  { name: 'non-canonical signature (S + L)', token: nonCanonicalS(year), line_date: '2027-03-01', today: '2027-03-01', status: 'invalid', codes: ['bad_signature'] },
  { name: 'signed by an unknown kid', token: signLicense(payload({ kid: other.kid }), other), line_date: '2027-03-01', today: '2027-03-01', status: 'invalid', codes: ['unknown_kid'] },
  { name: "kid of a known key, signed by another key", token: signLicense(payload(), other), line_date: '2027-03-01', today: '2027-03-01', status: 'invalid', codes: ['bad_signature'] },
  { name: 'line after updates_until warns', token: year, line_date: '2027-11-15', today: '2028-01-01', status: 'warning', codes: ['updates_expired'],
    console: "License warning: updates for this version line ended 2027-10-01 (this build's line is from 2027-11-15) (license L-test, Example LAN AS)",
    player: 'Licensed to Example LAN AS' },
  { name: 'later patch of a covered line stays covered', token: year, line_date: '2027-09-01', today: '2028-06-01', status: 'ok', codes: [] },
  { name: 'last covered day is inclusive', token: year, line_date: '2027-10-01', today: '2027-10-02', status: 'ok', codes: [] },
  { name: 'event window, inside', token: event, line_date: '2026-09-28', today: '2026-10-15', status: 'ok', codes: [],
    console: 'License: NTLAN · Servers S (6 servers) · event 2026-10-14 to 2026-10-16 · valid (license L-test)' },
  { name: 'event window ended', token: event, line_date: '2026-09-28', today: '2026-10-17', status: 'warning', codes: ['period_ended'] },
  { name: 'event window not started', token: event, line_date: '2026-09-28', today: '2026-10-13', status: 'warning', codes: ['period_not_started'] },
  { name: 'event, both warnings', token: event, line_date: '2026-11-01', today: '2026-11-02', status: 'warning', codes: ['updates_expired', 'period_ended'] },
  { name: 'founder covers every line', token: signLicense(payload({ kind: 'founder', updates_until: '9999-12-31' }), key), line_date: '2040-01-01', today: '2041-01-01', status: 'ok', codes: [] },
  { name: 'platform key covers Ready Up too', token: signLicense(payload({ product: 'platform', pack: 'L', max_servers: 40 }), key), line_date: '2027-03-01', today: '2027-03-01', status: 'ok', codes: [] },
  { name: 'no licensee: nothing for players', token: signLicense(withoutLicensee(payload()), key), line_date: '2027-03-01', today: '2027-03-01', status: 'ok', codes: [],
    console: 'License: Servers M (15 servers) · yearly, updates until 2027-10-01 · valid (license L-test)', player: '' },
  { name: 'UTF-8 licensee', token: signLicense(payload({ licensee: 'Ålesund LAN Øst' }), key), line_date: '2027-03-01', today: '2027-03-01', status: 'ok', codes: [], player: 'Licensed to Ålesund LAN Øst' },
  { name: 'unknown fields are ignored', token: signLicense({ ...payload(), future_field: { a: [1, 2] } }, key), line_date: '2027-03-01', today: '2027-03-01', status: 'ok', codes: [] },
  { name: 'signed, max_servers 0', token: signLicense(payload({ max_servers: 0 }), key), line_date: '2027-03-01', today: '2027-03-01', status: 'invalid', codes: ['malformed'] },
  { name: 'signed, max_servers 1.5', token: signLicense(payload({ max_servers: 1.5 }), key), line_date: '2027-03-01', today: '2027-03-01', status: 'invalid', codes: ['malformed'] },
  { name: 'signed, product other', token: signLicense(payload({ product: 'other' }), key), line_date: '2027-03-01', today: '2027-03-01', status: 'invalid', codes: ['malformed'] },
  { name: 'signed, bad pack', token: signLicense(payload({ pack: 'XL' }), key), line_date: '2027-03-01', today: '2027-03-01', status: 'invalid', codes: ['malformed'] },
  { name: 'monthly key', token: signLicense(payload({ kind: 'month', updates_until: '2027-03-15' }), key), line_date: '2027-03-01', today: '2027-03-01', status: 'ok', codes: [],
    console: 'License: Example LAN AS · Servers M (15 servers) · monthly, paid until 2027-03-15 · valid (license L-test)' },
  { name: 'signed, bad kind', token: signLicense(payload({ kind: 'weekly' }), key), line_date: '2027-03-01', today: '2027-03-01', status: 'invalid', codes: ['malformed'] },
  { name: 'signed, missing id', token: signLicense(payload({ id: '' }), key), line_date: '2027-03-01', today: '2027-03-01', status: 'invalid', codes: ['malformed'] },
  { name: 'signed, missing customer', token: signLicense({ ...payload(), customer: undefined }, key), line_date: '2027-03-01', today: '2027-03-01', status: 'invalid', codes: ['malformed'] },
  { name: 'signed, impossible updates_until', token: signLicense(payload({ updates_until: '2027-02-30' }), key), line_date: '2027-03-01', today: '2027-03-01', status: 'invalid', codes: ['malformed'] },
  { name: 'signed, bad issued_at', token: signLicense(payload({ issued_at: 'yesterday' }), key), line_date: '2027-03-01', today: '2027-03-01', status: 'invalid', codes: ['malformed'] },
  { name: 'signed, valid_from without valid_to', token: signLicense(payload({ valid_from: '2026-10-14' }), key), line_date: '2027-03-01', today: '2027-03-01', status: 'invalid', codes: ['malformed'] },
  { name: 'signed, window backwards', token: signLicense(payload({ valid_from: '2026-10-16', valid_to: '2026-10-14' }), key), line_date: '2027-03-01', today: '2027-03-01', status: 'invalid', codes: ['malformed'] },
  { name: 'signed, lease not a boolean', token: signLicense(payload({ lease: 'yes' }), key), line_date: '2027-03-01', today: '2027-03-01', status: 'invalid', codes: ['malformed'] },
  { name: 'signed, licensee not a string', token: signLicense(payload({ licensee: 42 }), key), line_date: '2027-03-01', today: '2027-03-01', status: 'invalid', codes: ['malformed'] },
  { name: 'signed, v missing', token: signLicense({ ...payload(), v: undefined }, key), line_date: '2027-03-01', today: '2027-03-01', status: 'invalid', codes: ['malformed'] },
  { name: 'version 2 needs a newer release', token: `${PREFIX}.${v2}.${'A'.repeat(86)}`, line_date: '2027-03-01', today: '2027-03-01', status: 'invalid', codes: ['unsupported_version'] },
  { name: 'empty', token: '', line_date: '2027-03-01', today: '2027-03-01', status: 'invalid', codes: ['malformed'] },
  { name: 'not a key', token: 'hello', line_date: '2027-03-01', today: '2027-03-01', status: 'invalid', codes: ['malformed'] },
  { name: 'empty parts', token: 'ATL1..', line_date: '2027-03-01', today: '2027-03-01', status: 'invalid', codes: ['malformed'] },
  { name: 'other prefix', token: `ATL2.${yearBody}.${yearSig}`, line_date: '2027-03-01', today: '2027-03-01', status: 'invalid', codes: ['malformed'] },
  { name: 'four parts', token: `${year}.x`, line_date: '2027-03-01', today: '2027-03-01', status: 'invalid', codes: ['malformed'] },
  { name: 'too long', token: `ATL1.${'a'.repeat(5000)}.b`, line_date: '2027-03-01', today: '2027-03-01', status: 'invalid', codes: ['malformed'] },
  { name: 'not base64url', token: 'ATL1.!!!.???', line_date: '2027-03-01', today: '2027-03-01', status: 'invalid', codes: ['malformed'] },
  { name: 'standard base64 padding', token: `${year}==`, line_date: '2027-03-01', today: '2027-03-01', status: 'invalid', codes: ['malformed'] },
  { name: 'payload not JSON', token: `ATL1.${Buffer.from('{nope').toString('base64url')}.${yearSig}`, line_date: '2027-03-01', today: '2027-03-01', status: 'invalid', codes: ['malformed'] },
  { name: 'short signature', token: `ATL1.${yearBody}.${yearSig.slice(0, 80)}`, line_date: '2027-03-01', today: '2027-03-01', status: 'invalid', codes: ['malformed'] },
];

const args = process.argv.slice(2);
const refIndex = args.indexOf('--reference');
if (refIndex >= 0) {
  const ref = await import(pathToFileURL(path.resolve(args[refIndex + 1])).href);
  const publicKeys = { [key.kid]: { kty: 'OKP', crv: 'Ed25519', x: key.x } };
  let bad = 0;
  for (const c of cases) {
    const r = ref.verifyLicense(c.token, { publicKeys, lineDate: c.line_date, now: c.today });
    const codes = r.warnings.map((w) => w.code);
    if (r.status !== c.status || JSON.stringify(codes) !== JSON.stringify(c.codes)) {
      bad++;
      console.error(`reference disagrees: ${c.name}: ${r.status} ${JSON.stringify(codes)}`);
    }
  }
  console.error(`reference verifier: ${cases.length - bad}/${cases.length} cases agree`);
  if (bad) process.exit(1);
}

process.stdout.write(
  `${JSON.stringify(
    {
      note: 'Generated by tests/license/make_vectors.mjs with a throwaway key pair (private key discarded). Do not edit by hand.',
      keys: [{ kid: key.kid, x: key.x }],
      cases,
      // Paid license standing (license.h StandingFor): a monthly key paid until 2027-03-15, its
      // lease (20 servers, paid until 2027-04-15), and terms without the lease mark.
      standing: {
        key: signLicense(payload({ kind: 'month', max_servers: 10, updates_until: '2027-03-15', issued_at: '2027-02-15T10:00:00Z' }), key),
        lease: signLicense(payload({ kind: 'month', max_servers: 20, updates_until: '2027-04-15', issued_at: '2027-03-15T10:00:00Z', lease: true }), key),
        unmarked: signLicense(payload({ kind: 'month', max_servers: 20, updates_until: '2027-04-15', issued_at: '2027-03-15T10:00:00Z' }), key),
      },
    },
    null,
    2,
  )}\n`,
);
