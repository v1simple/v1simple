/**
 * Deploy script - copies built SvelteKit files to ../data/ for LittleFS
 */
import { cpSync, rmSync, existsSync, mkdirSync, readdirSync, statSync } from 'fs';
import { spawnSync } from 'child_process';
import { join, dirname } from 'path';
import { fileURLToPath } from 'url';
import { stageAudioFiles } from './audio-manifest.js';

const __dirname = dirname(fileURLToPath(import.meta.url));
const buildDir = join(__dirname, '..', 'build');
const dataDir = join(__dirname, '..', '..', 'data');

console.log('🚀 Deploying SvelteKit build to LittleFS data folder...');

// Check if build exists
if (!existsSync(buildDir)) {
    console.error('❌ Build folder not found! Run "npm run build" first.');
    process.exit(1);
}

// Clear data folder (except any non-web files we want to keep)
if (existsSync(dataDir)) {
    console.log('🧹 Clearing existing data folder...');
    rmSync(dataDir, { recursive: true });
}
mkdirSync(dataDir, { recursive: true });

// Copy build to data
console.log('📁 Copying build files...');
cpSync(buildDir, dataDir, { recursive: true });

// Keep only compressed runtime assets under /_app when a .gz twin exists.
// This saves flash while preserving uncompressed HTML entry pages.
function pruneUncompressedAppAssets(appDir) {
    if (!existsSync(appDir)) {
        return { removedFiles: 0, removedBytes: 0 };
    }

    const stack = [appDir];
    let removedFiles = 0;
    let removedBytes = 0;
    const compressibleExt = /\.(js|css|json)$/;
    const keepRawFiles = new Set([join(appDir, 'env.js'), join(appDir, 'version.json')]);

    while (stack.length > 0) {
        const dir = stack.pop();
        for (const file of readdirSync(dir)) {
            const filePath = join(dir, file);
            const stat = statSync(filePath);

            if (stat.isDirectory()) {
                stack.push(filePath);
                continue;
            }

            if (!compressibleExt.test(file) || file.endsWith('.gz')) {
                continue;
            }

            if (keepRawFiles.has(filePath)) {
                continue;
            }

            const gzPath = `${filePath}.gz`;
            if (!existsSync(gzPath)) {
                continue;
            }

            rmSync(filePath);
            removedFiles++;
            removedBytes += stat.size;
        }
    }

    return { removedFiles, removedBytes };
}

const pruned = pruneUncompressedAppAssets(join(dataDir, '_app'));
if (pruned.removedFiles > 0) {
    console.log(
        `🗜️ Pruned ${pruned.removedFiles} uncompressed /_app assets (${(pruned.removedBytes / 1024).toFixed(1)} KB)`
    );
}

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

// List deployed files with sizes
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

function platformioInfo() {
    const result = spawnSync(process.env.PIO_CMD ?? 'pio', ['system', 'info', '--json-output'], {
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
        throw new Error(`could not parse PlatformIO system info: ${error.message}`);
    }
}

function runLittleFsCapacityGate(rawBytes) {
    console.log('\n📦 LittleFS capacity gate:');

    let info;
    try {
        info = platformioInfo();
    } catch (error) {
        console.error(`❌ LittleFS capacity gate: FAIL — ${error.message}`);
        process.exit(1);
    }

    const python = info.python_exe?.value;
    const coreDir = info.core_dir?.value;
    if (!python || !coreDir) {
        console.error('❌ LittleFS capacity gate: FAIL — PlatformIO did not report its Python executable and core directory');
        process.exit(1);
    }

    const result = spawnSync(
        python,
        [
            join(__dirname, 'littlefs-capacity.py'),
            '--data-dir',
            dataDir,
            '--partition-table',
            join(__dirname, '..', '..', 'partitions_v1.csv'),
            '--platformio-core-dir',
            coreDir,
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

console.log('\n💡 Next steps:');
console.log('   cd .. && ./build.sh --upload-fs');
