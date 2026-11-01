const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const crypto = require('node:crypto');
const { execFileSync } = require('node:child_process');
test('engine receipt records clean commit, actual binary hash, MIDI option and dirty changes', t => {
    const root = fs.mkdtempSync(path.join(os.tmpdir(), 'timox-engine-build-'));
    t.after(() => fs.rmSync(root, {recursive:true, force:true}));
    const source = path.join(root, 'source'); fs.mkdirSync(source);
    const git = (...args) => execFileSync('git', ['-C', source, ...args], {encoding:'utf8'}).trim();
    fs.writeFileSync(path.join(source, 'source.txt'), 'source');
    git('init', '-q'); git('add', '.');
    git('-c', 'user.name=Build test', '-c', 'user.email=test@example.invalid', 'commit', '-qm', 'fixture');
    const binary = path.join(root, 'engine.exe'); fs.writeFileSync(binary, 'fixture binary');
    const build = midi => {
        execFileSync(process.env.CMAKE_COMMAND || 'cmake', [`-DSOURCE_ROOT=${source}`, `-DENGINE_BINARY=${binary}`,
            `-DMIDI_ENABLED=${midi}`, '-P', path.resolve(__dirname, '../cmake/WriteBuildInfo.cmake')]);
        return JSON.parse(fs.readFileSync(path.join(root, 'engine-build.json')));
    };
    const clean = build('ON');
    assert.equal(clean.revision, git('rev-parse', 'HEAD'));
    assert.equal(clean.dirty, false);
    assert.equal(clean.windowsMidiServices, true);
    assert.equal(clean.binarySha256, crypto.createHash('sha256').update('fixture binary').digest('hex'));
    assert.equal(build('OFF').windowsMidiServices, false);
    fs.writeFileSync(path.join(source, 'source.txt'), 'changed');
    assert.equal(build('ON').dirty, true);
});
