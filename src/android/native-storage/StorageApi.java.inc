    // Discover candidates from immutable private producer records, not ROMs, the current catalog,
    // or a removable volume's directory names. compiledInventory still authenticates every byte.
    static Map<String, java.util.Set<String>> storageOwners(File files, String engine) throws IOException, JSONException {
        Map<String, java.util.Set<String>> owners = new LinkedHashMap<>();
        for (boolean input : new boolean[] {false, true}) {
            File controls = new File(files, "native-" + engine + (input ? "-input-control" : "-control"));
            for (File directory : existingChildren(controls)) {
                if (!directory.getName().matches(input ? "[0-9a-f]{64}" : GENERATION)) continue;
                File record = new File(directory, "identity.json");
                CompiledFiles.plain(record);
                if (!record.exists()) continue; // failed reservation; never enroll removable-volume data
                JSONObject job = ownedJson(record);
                JSONObject owner = input ? job : job.getJSONObject("embedded");
                String title = owner.getString("title_id"), source = owner.getString("original_path");
                if (!title.matches("[0-9A-F]{16}") || !title.equals(job.getString("title_id")) || source.isEmpty())
                    throw new IOException("Invalid private native storage identity.");
                owners.computeIfAbsent(title, ignored -> new java.util.LinkedHashSet<>()).add(source);
            }
        }
        return owners;
    }

    /** Returned rows describe retained data, not an estimate of physical space freed (e.g. hard links).
     * Destructive callers must also hold host export and launch admission throughout this call. */
    public static String managedStorage(Context c, String selectedTitle, boolean purge) throws IOException {
        if (!BuildConfig.NX_EMBEDDED || !"native".equals(BuildConfig.NX_PACK_ENGINE)
                || selectedTitle != null && !selectedTitle.matches("[0-9A-F]{16}"))
            throw new IOException("Invalid native storage request.");
        // ENGINE_INIT
        if (Prefs.open(c, "embedded_prepare").getString("storage", null) == null) return "[]";
        return locked(c, () -> {
            Map<String, java.util.Set<String>> owners = storageOwners(c.getFilesDir().getCanonicalFile(), "ENGINE_NAME");
            java.util.Set<String> sources = new java.util.LinkedHashSet<>();
            for (java.util.Set<String> paths : owners.values()) sources.addAll(paths);
            // An original registered to the other engine must not be swallowed by this engine's tree.
            if (purge) {
                for (java.util.Set<String> paths : storageOwners(c.getFilesDir().getCanonicalFile(), "OTHER_ENGINE").values())
                    sources.addAll(paths);
            }
            org.json.JSONArray rows = new org.json.JSONArray();
            for (String title : owners.keySet()) {
                if (selectedTitle != null && !selectedTitle.equals(title)) continue;
                JSONObject row = new JSONObject().put("title_id", title);
                try {
                    CompiledInventory data = compiledInventory(c, null, title, sources);
                    if (!data.files.present()) continue; // do not list retained control history as game data
                    row.put("bytes", Math.addExact(data.files.bytes(), data.files.inputBytes()));
                    row.put("busy", data.busy || purge && !deleteIdleInventory(c, data));
                } catch (Exception failure) {
                    row.put("error", failure.getMessage() == null ? failure.toString() : failure.getMessage());
                }
                rows.put(row);
            }
            return rows.toString();
        });
    }
