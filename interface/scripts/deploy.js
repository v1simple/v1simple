import { rmSync, existsSync, mkdirSync, readdirSync, statSync } from 'fs';
import { spawnSync } from 'child_process';
import { join, dirname } from 'path';
import { fileURLToPath } from 'url';
import { stageAudioFiles } from './audio-manifest.js';

const __dirname = dirname(fileURLToPath(import.meta.url));
const dataDir = join(__dirname, '..', '..', 'data');

console.log('🚀 Staging audio in the LittleFS data folder...');

if (existsSync(dataDir)) {
    console.log('🧹 Clearing existing data folder...');
    rmSync(dataDir, { recursive: true });
}
mkdirSync(dataDir, { recursive: true });

// The current web UI is embedded in firmware. Stage only filesystem-owned
// audio; managed pages/assets never fall back to the LittleFS copy.
const stagedAudio = stageAudioFiles();
if (stagedAudio.missing.length > 0) {
    console.error(
        `❌ Missing ${stagedAudio.missing.length} audio clips required by ${stagedAudio.sourceDir}:`
    );
    for (const file of stagedAudio.missing) {
        console.error(`   ${file}`);
    }
    process.exit(1);
}
console.log(
    `🔊 Staged ${stagedAudio.copied}/${stagedAudio.expected} audio clips to ${stagedAudio.targetDir}`
);

function listFiles(dir, prefix = '') {
    const files = readdirSync(dir);
    let totalSize = 0;

    for (const file of files) {
        const filePath = join(dir, file);
        const stat = statSync(filePath);

        if (stat.isDirectory()) {
            totalSize += listFiles(filePath, prefix + file + '/');
        } else {
            const size = stat.size;
            totalSize += size;
            const sizeStr = size > 1024 ? `${(size / 1024).toFixed(1)} KB` : `${size} B`;
            console.log(`   ${prefix}${file} (${sizeStr})`);
        }
    }
    return totalSize;
}

console.log('\n📄 Deployed files:');
const totalSize = listFiles(dataDir);
console.log(`\n✅ Total size: ${(totalSize / 1024).toFixed(1)} KB`);

function selectedBuildContext() {
    const result = spawnSync(process.env.PYTHON_CMD ?? 'python3', [
        join(__dirname, '..', '..', 'scripts', 'build_context.py'),
        '--pio', process.env.PIO_CMD ?? 'pio', '--env', process.env.PIO_ENV ?? 'waveshare-349'
    ], {
        cwd: join(__dirname, '..', '..'),
        encoding: 'utf8'
    });

    if (result.error || result.status !== 0) {
        const detail = result.error?.message ?? result.stderr?.trim() ?? `exit ${result.status}`;
        throw new Error(`could not inspect PlatformIO: ${detail}`);
    }

    try {
        return JSON.parse(result.stdout);
    } catch (error) {
        throw new Error(`could not parse PlatformIO system info: ${error.message}`, { cause: error });
    }
}

function runLittleFsCapacityGate(rawBytes) {
    console.log('\n📦 LittleFS capacity gate:');

    let info;
    try {
        info = selectedBuildContext();
    } catch (error) {
        console.error(`❌ LittleFS capacity gate: FAIL — ${error.message}`);
        process.exit(1);
    }

    const python = info.tools?.python;
    if (!python) {
        console.error('❌ LittleFS capacity gate: FAIL — PlatformIO did not report its Python executable and core directory');
        process.exit(1);
    }

    const result = spawnSync(
        python,
        [
            join(__dirname, 'littlefs-capacity.py'),
            '--data-dir',
            dataDir,
            '--context-json',
            JSON.stringify(info),
            '--expected-raw-bytes',
            String(rawBytes)
        ],
        { cwd: join(__dirname, '..', '..'), stdio: 'inherit' }
    );

    if (result.error) {
        console.error(`❌ LittleFS capacity gate: FAIL — could not run packer: ${result.error.message}`);
        process.exit(1);
    }
    if (result.status !== 0) {
        process.exit(result.status ?? 1);
    }
}

runLittleFsCapacityGate(totalSize);
