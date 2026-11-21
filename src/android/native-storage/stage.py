"""Extend the two pinned embedded hosts without editing either checkout; called by hosts.gradle."""
import argparse
from pathlib import Path
from textwrap import dedent, indent


def once(text, old, new):
    if text.count(old) != 1:
        raise ValueError(f"Embedded host source changed: expected one {old!r}")
    return text.replace(old, new)


def compiled_files(text):
    text = once(text, 'private final Path root, original;', '''private final Path root;
    private final java.util.Set<Path> originals = new java.util.HashSet<>();''')
    text = once(text, 'this.root = plain(root);', '''this.root = plain(root);
        protectOriginal(original);
    }

    void protectOriginal(String original) throws IOException {''')
    text = once(text, 'this.original = source.isAbsolute() ? source.getCanonicalFile().toPath() : null;', '''if (source.isAbsolute()) {
            Path path = source.getCanonicalFile().toPath();
            if (path.startsWith(root) || root.startsWith(path)) originals.add(path);
        }''')
    return once(text, 'if (original != null && (original.startsWith(path) || path.startsWith(original)))\n            throw',
                'for (Path original : originals) if (original.startsWith(path) || path.startsWith(original))\n            throw')


def embedded_game(text, engine):
    # Reuse the actual deletion transaction, including retire-before-unlink, inherited compiler
    # leases and publication checks. Never duplicate this filesystem policy in a frontend.
    start = text.index('            if (!data.files.present() && data.inputs.isEmpty()) return null;')
    end = text.index('            return null;\n        });\n    }', start)
    block = text[start:end]
    body = once(block, 'if (!data.files.present() && data.inputs.isEmpty()) return null;',
                'if (!data.files.present() && data.inputs.isEmpty()) return true;')
    for reason in ('A native input export is still running.',
                   'A native preparation worker or compiler descendant is still running.'):
        body = once(body, f'if (lease == null) throw new IOException("{reason}");', 'if (lease == null) return false;')
    body = once(body, 'if (running(c)) throw new IOException("Stop native gameplay before deleting compiled data.");',
                'if (running(c)) return false;')
    text = once(text, block + '            return null;\n        });\n    }', '''            if (!deleteIdleInventory(c, data)) throw new IOException("Native preparation or gameplay still owns this data.");
            return null;
        });
    }

    private static boolean deleteIdleInventory(Context c, CompiledInventory data) throws IOException {
''' + indent(dedent(body), '        ') + '        return true;\n    }')

    signature = 'private static CompiledInventory compiledInventory(Context c, String original, String title) throws IOException, JSONException {'
    text = once(text, signature, signature + '''
        return compiledInventory(c, original, title, java.util.Collections.emptySet());
    }

    private static CompiledInventory compiledInventory(Context c, String original, String title, java.util.Set<String> sources)
            throws IOException, JSONException {''')
    text = once(text, 'Map<String, Job> generations = new LinkedHashMap<>();', '''for (String source : sources) data.files.protectOriginal(source);
        Map<String, Job> generations = new LinkedHashMap<>();''')
    for other in ('source', 'job.embeddedInfo().getString("original_path")', 'owner.embeddedInfo().getString("original_path")'):
        text = once(text, f'if (!original.equals({other}))', f'if (original != null && !original.equals({other}))')
    if f'new File(storage(c), "native/{engine}")' not in text:
        raise ValueError(f'Not the {engine} embedded host')
    api = Path(__file__).with_name('StorageApi.java.inc').read_text(encoding='utf-8')
    init = 'EmbeddedRuntime.initialize(c);' if engine == 'banana' else ''
    api = api.replace('// ENGINE_INIT', init).replace('ENGINE_NAME', engine)
    api = api.replace('OTHER_ENGINE', 'peanuts' if engine == 'banana' else 'banana')
    return once(text, '    private static void maintenanceTitle(', api + '    private static void maintenanceTitle(')


def game_activity(text):
    # The embedded player owns a separate task, not a reusable launcher. Keep live games
    # resumable, but never reuse a dead process's one-shot save handoff from Recents.
    start = '        ExitWatch.game(this, id,'
    text = once(text, start, '''        if (!hostRunning && (getIntent().getFlags() & android.content.Intent.FLAG_ACTIVITY_LAUNCHED_FROM_HISTORY) != 0) {
            finishAndRemoveTask();
            return;
        }
''' + start)
    text = once(text, 'ExitWatch.startFailed(why);\n            finish();',
                'ExitWatch.startFailed(why);\n            finishAndRemoveTask();')
    return once(text, 'backToFrontend();\n        finish();', 'backToFrontend();\n        finishAndRemoveTask();')


def stage(engine, roots, output):
    for name, transform in [('CompiledFiles', compiled_files), ('EmbeddedGame', lambda s: embedded_game(s, engine)),
                            ('GameActivity', game_activity)]:
        paths = [(root, path) for root in roots for path in root.glob(f'dev/*/android/{name}.java')]
        if len(paths) != 1:
            raise ValueError(f'Expected one {name}.java in the embedded Java source roots; found {paths}')
        root, path = paths[0]
        dest = output / path.relative_to(root)
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_text(transform(path.read_text(encoding='utf-8')), encoding='utf-8')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('engine', choices=['peanuts', 'banana'])
    parser.add_argument('output', type=Path)
    parser.add_argument('roots', type=Path, nargs='+')
    args = parser.parse_args()
    stage(args.engine, args.roots, args.output)
