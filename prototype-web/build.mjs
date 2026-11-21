import { copyFile, mkdir } from 'node:fs/promises';
import { build } from 'esbuild';

const root = new URL('./', import.meta.url);
await mkdir(new URL('dist/', root), { recursive: true });

await Promise.all([
  build({
    absWorkingDir: import.meta.dirname,
    entryPoints: ['app.js'],
    outfile: 'dist/app.js',
    bundle: true,
    minify: true,
    format: 'esm',
    target: 'es2022',
  }),
  ...['index.html', 'styles.css'].map(file => copyFile(new URL(file, root), new URL(`dist/${file}`, root))),
  copyFile(new URL('node_modules/three/LICENSE', root), new URL('dist/three-LICENSE.txt', root)),
  copyFile(new URL('node_modules/lucide/LICENSE', root), new URL('dist/lucide-LICENSE.txt', root)),
]);
