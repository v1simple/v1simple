/**
 * Deploy script - copies built SvelteKit files to ../data/ for LittleFS
 */
import { cpSync, rmSync, existsSync, mkdirSync, readdirSync, statSync } from 'fs';
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

console.log('\n💡 Next steps:');
console.log('   cd .. && ./build.sh --upload-fs');
