const fs = require('node:fs');
const path = require('node:path');
const crypto = require('node:crypto');
const assert = require('node:assert/strict');
const root = __dirname;
const manifest = JSON.parse(fs.readFileSync(path.join(root, 'import-manifest.json')));
for (const entry of manifest.files) {
  const actual = crypto.createHash('sha256').update(fs.readFileSync(path.join(root, entry.target))).digest('hex');
  assert.equal(actual, entry.targetSha256, entry.target);
}
const lock = JSON.parse(fs.readFileSync(path.join(root, 'vendor/timoxvasio/dependency-lock.json')));
for (const entry of lock.files) {
  const actual = crypto.createHash('sha256').update(fs.readFileSync(path.join(root, entry.target))).digest('hex');
  assert.equal(actual, entry.sha256, `Canonical ABI changed: ${entry.target}`);
}
assert(!fs.readFileSync(path.join(root, 'src/pwar_client_supervisor.cpp'), 'utf8').includes('build_pwar_reconciled_tests'));
console.log(`Verified ${manifest.files.length} import entries and ${lock.files.length} canonical ABI pins.`);
