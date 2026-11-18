package dev.nxnative.android

import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test
import java.io.File
import java.io.IOException
import java.nio.file.Files
import java.io.ByteArrayInputStream
import java.io.ByteArrayOutputStream
import java.util.zip.ZipEntry
import java.util.zip.ZipOutputStream

class EmbeddedCacheRoutingTest {
    private val title = "0100F7E00C70E000"

    private fun job(work: File, embedded: Boolean = true, cache: File? = null): Job {
        val record = JSONObject().put("format", Job.FORMAT).put("title_id", title)
            .put("work", work.path).put("pack", File(work.parentFile, "pack.partial").path)
        if (embedded) record.put("embedded", JSONObject().put("generation", work.name))
        if (cache != null) record.put("shader_cache", cache.path)
        return Job.of(Job.fileOf(work), record.toString())!!
    }

    @Test fun embeddedCachesStayOnTheirOwnGenerationAcrossReopenAndCleanup() {
        val root = Files.createTempDirectory("embedded-caches").toFile()
        try {
            val internal = File(root, "internal").apply { mkdirs() }
            val first = job(File(root, "selected/jobs/first"))
            val second = job(File(root, "selected/jobs/second"))
            for (j in listOf(first, second)) {
                j.validateEmbeddedCaches()
                assertEquals(File(j.dir(), "split.scan"), j.splitScan(null))
                assertEquals(File(j.dir(), "ue"), j.ueShaders(null))
                assertEquals(File(j.dir(), "xlate"), j.results(null))
                assertEquals(File(j.dir(), "lib"), j.shaderCache())
            }
            val foreign = File(internal, "xlate/$title/foreign").apply { parentFile.mkdirs(); writeText("keep") }
            val other = File(second.results(null), "other").apply { parentFile.mkdirs(); writeText("keep") }
            val owned = File(first.results(null), "owned").apply { parentFile.mkdirs(); writeText("remove") }
            first.save()
            val reopened = Job.read(first.file)
            reopened.validateEmbeddedCaches()
            assertEquals(first.results(null), reopened.results(null))
            reopened.deleteResults(null)
            assertFalse(owned.exists())
            assertTrue(other.isFile)
            assertTrue(foreign.isFile)
        } finally { root.deleteRecursively() }
    }

    @Test fun legacyAndReopenedEmbeddedJobsRejectForeignShaderCacheMetadata() {
        val root = Files.createTempDirectory("embedded-cache-metadata").toFile()
        try {
            val work = File(root, "selected/jobs/current")
            val legacy = job(work)
            legacy.validateEmbeddedCaches()
            assertEquals(File(work, "ondevice/lib"), legacy.shaderCache())
            for (cache in listOf(File(root, "internal/xlate/$title"), File(root, "selected/jobs/previous/ondevice/lib"))) {
                val altered = job(work, cache = cache)
                altered.save()
                val reopened = Job.read(altered.file)
                assertThrows(IOException::class.java) { reopened.validateEmbeddedCaches() }
                assertThrows(IllegalStateException::class.java) { reopened.shaderCache() }
            }
        } finally { root.deleteRecursively() }
    }

    @Test fun standaloneStillAllowsExplicitShaderReuse() {
        val reused = File("existing-pack/shaders")
        val j = job(File("dump-work"), embedded = false, cache = reused)
        j.validateEmbeddedCaches()
        assertEquals(reused, j.shaderCache())
    }

    @Test fun internalToolEstimateCountsUnpackedFilesRatherThanCompressedArchiveSize() {
        val output = ByteArrayOutputStream()
        ZipOutputStream(output).use { zip ->
            zip.putNextEntry(ZipEntry("python/"))
            zip.closeEntry()
            zip.putNextEntry(ZipEntry("python/small.py"))
            zip.write(byteArrayOf(1))
            zip.closeEntry()
            zip.putNextEntry(ZipEntry("python/large.py"))
            zip.write(ByteArray(4097))
            zip.closeEntry()
        }
        assertEquals(16384L, EmbeddedGame.toolArchiveBytes(ByteArrayInputStream(output.toByteArray())))
    }
}
