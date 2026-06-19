import { cpSync, existsSync, mkdirSync, readFileSync, readdirSync, rmSync } from 'fs';
import { dirname, join } from 'path';
import { fileURLToPath } from 'url';

const __dirname = dirname(fileURLToPath(import.meta.url));

export function resolveProjectRoot() {
	return join(__dirname, '..', '..');
}

export function loadAudioManifest(projectRoot = resolveProjectRoot()) {
	const manifestPath = join(projectRoot, 'config', 'audio_asset_manifest.json');
	const manifest = JSON.parse(readFileSync(manifestPath, 'utf8'));
	const files = new Set(manifest.files ?? []);

	for (const range of manifest.ranges ?? []) {
		const prefix = range.prefix ?? '';
		const suffix = range.suffix ?? '';
		const width = Number(range.width ?? 0);
		for (let value = Number(range.start ?? 0); value <= Number(range.end ?? -1); value++) {
			files.add(`${prefix}${String(value).padStart(width, '0')}${suffix}`);
		}
	}

	return {
		manifestPath,
		files: [...files].sort()
	};
}

export function stageAudioFiles({
	projectRoot = resolveProjectRoot(),
	sourceDir = join(projectRoot, 'tools', 'freq_audio', 'mulaw'),
	targetDir = join(projectRoot, 'data', 'audio')
} = {}) {
	const { files } = loadAudioManifest(projectRoot);

	mkdirSync(targetDir, { recursive: true });
	for (const entry of readdirSync(targetDir)) {
		if (entry.endsWith('.mul')) {
			rmSync(join(targetDir, entry));
		}
	}

	const missing = [];
	let copied = 0;
	for (const file of files) {
		const sourcePath = join(sourceDir, file);
		if (!existsSync(sourcePath)) {
			missing.push(file);
			continue;
		}
		cpSync(sourcePath, join(targetDir, file));
		copied++;
	}

	return { copied, expected: files.length, missing, sourceDir, targetDir };
}
