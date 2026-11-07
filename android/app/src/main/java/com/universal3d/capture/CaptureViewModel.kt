package com.universal3d.capture

import android.app.Application
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.net.Uri
import android.os.Environment
import android.provider.OpenableColumns
import android.webkit.MimeTypeMap
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.File
import java.io.IOException
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

data class UiState(
    val photoCount: Int = 0,
    val pickedCount: Int = 0,
    val busy: Boolean = false,
    val status: String? = null,
    /** 0..1000, or null to hide the progress bar. */
    val progress: Int? = null,
)

/**
 * Holds the capture session (photos survive rotation and app restarts) and runs the upload +
 * progress polling against the reconstruction server.
 */
class CaptureViewModel(app: Application) : AndroidViewModel(app) {

    private val _state = MutableStateFlow(UiState())
    val state: StateFlow<UiState> = _state.asStateFlow()

    private val sessionDir = File(
        app.getExternalFilesDir(Environment.DIRECTORY_PICTURES) ?: app.filesDir, "session"
    )
    private val captured = mutableListOf<File>()
    private val picked = mutableListOf<Uri>()
    private var work: Job? = null

    init {
        sessionDir.listFiles { f -> f.extension.equals("jpg", ignoreCase = true) && f.length() > 0 }
            ?.sortedBy { it.name }
            ?.let { captured.addAll(it) }
        publishCounts()
    }

    fun newPhotoFile(): File {
        sessionDir.mkdirs()
        val stamp = SimpleDateFormat("yyyyMMdd_HHmmss_SSS", Locale.US).format(Date())
        return File(sessionDir, "IMG_$stamp.jpg")
    }

    fun onPhotoSaved(file: File) {
        captured += file
        publishCounts()
    }

    fun addPicked(uris: List<Uri>) {
        uris.filterNot { it in picked }.let { picked += it }
        publishCounts()
    }

    fun clear() {
        if (_state.value.busy) return
        captured.forEach { it.delete() }
        captured.clear()
        picked.clear()
        _state.update { it.copy(status = null, progress = null) }
        publishCounts()
    }

    fun checkServer(rawUrl: String) {
        val url = ServerUrl.normalize(rawUrl) ?: return setStatus("Enter the server address, e.g. 192.168.1.20:8000")
        viewModelScope.launch {
            setStatus("Connecting to $url…")
            val result = withContext(Dispatchers.IO) { runCatching { ReconApi(url).info() } }
            result.onSuccess { info ->
                val dry = if (info.optBoolean("dry_run")) " (dry-run mode)" else ""
                setStatus("Connected to ${info.optString("name")} ${info.optString("version")}$dry")
            }.onFailure { setStatus("Cannot reach $url: ${it.message}") }
        }
    }

    fun upload(rawUrl: String, method: String, iterations: Int) {
        if (_state.value.busy) return
        val url = ServerUrl.normalize(rawUrl) ?: return setStatus("Enter the server address, e.g. 192.168.1.20:8000")
        val files = captured.toList()
        val uris = picked.toList()
        val total = files.size + uris.size
        if (total == 0) return setStatus("Take or pick some photos first.")

        work?.cancel()
        work = viewModelScope.launch {
            _state.update { it.copy(busy = true, progress = 0, status = "Creating job…") }
            try {
                val api = ReconApi(url)
                val name = "Phone scan " + SimpleDateFormat("yyyy-MM-dd HH:mm", Locale.US).format(Date())
                val job = withContext(Dispatchers.IO) { api.createJob(name, method, iterations) }

                var done = 0
                for (file in files) {
                    withContext(Dispatchers.IO) { api.uploadFile(job.id, file.name, file, "image/jpeg") }
                    done++
                    reportUpload(done, total)
                }
                for ((index, uri) in uris.withIndex()) {
                    withContext(Dispatchers.IO) {
                        val (file, mime) = materialize(uri, index)
                        try {
                            api.uploadFile(job.id, file.name, file, mime)
                        } finally {
                            file.delete()
                        }
                    }
                    done++
                    reportUpload(done, total)
                }

                withContext(Dispatchers.IO) { api.startJob(job.id) }
                _state.update { it.copy(busy = false, status = "Uploaded $total file(s). Reconstruction started.") }
                poll(api, job.id)
            } catch (e: Exception) {
                if (e is kotlinx.coroutines.CancellationException) throw e
                _state.update { it.copy(busy = false, progress = null, status = "Upload failed: ${e.message}") }
            }
        }
    }

    private suspend fun poll(api: ReconApi, jobId: String) {
        var failures = 0
        while (viewModelScope.isActive) {
            delay(2000)
            val job = withContext(Dispatchers.IO) { runCatching { api.job(jobId) } }.getOrNull()
            if (job == null) {
                if (++failures >= 5) {
                    setStatus("Lost contact with the server; check U3D Studio for progress.")
                    _state.update { it.copy(progress = null) }
                    return
                }
                continue
            }
            failures = 0
            _state.update {
                it.copy(
                    status = JobText.describe(job.status, job.message, job.progress),
                    progress = if (job.isFinished && job.status != "succeeded") null else JobText.progressPermille(job.progress),
                )
            }
            if (job.isFinished) return
        }
    }

    private fun reportUpload(done: Int, total: Int) {
        _state.update {
            it.copy(progress = done * 1000 / total, status = "Uploading $done / $total…")
        }
    }

    /**
     * Copies a gallery item into the cache so it can be sent with a known length. HEIC/HEIF/WebP
     * photos are converted to JPEG, which COLMAP can read.
     */
    private fun materialize(uri: Uri, index: Int): Pair<File, String> {
        val app = getApplication<Application>()
        val resolver = app.contentResolver
        val mime = resolver.getType(uri) ?: "application/octet-stream"
        val displayName = queryDisplayName(uri) ?: "media_$index"
        val cache = File(app.cacheDir, "upload").apply { mkdirs() }
        val prefix = "G%04d_".format(Locale.US, index)

        if (mime in CONVERT_TO_JPEG) {
            val bitmap = resolver.openInputStream(uri)?.use { BitmapFactory.decodeStream(it) }
                ?: throw IOException("Cannot decode $displayName (unsupported on this Android version)")
            val out = File(cache, prefix + displayName.substringBeforeLast('.') + ".jpg")
            out.outputStream().use { bitmap.compress(Bitmap.CompressFormat.JPEG, 95, it) }
            bitmap.recycle()
            return out to "image/jpeg"
        }

        var name = displayName
        if (!name.contains('.')) {
            val ext = MimeTypeMap.getSingleton().getExtensionFromMimeType(mime) ?: "jpg"
            name += ".$ext"
        }
        val out = File(cache, prefix + name)
        resolver.openInputStream(uri)?.use { input -> out.outputStream().use { input.copyTo(it) } }
            ?: throw IOException("Cannot read $displayName")
        return out to mime
    }

    private fun queryDisplayName(uri: Uri): String? =
        getApplication<Application>().contentResolver
            .query(uri, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null)
            ?.use { c -> if (c.moveToFirst()) c.getString(0) else null }

    private fun publishCounts() {
        _state.update { it.copy(photoCount = captured.size, pickedCount = picked.size) }
    }

    private fun setStatus(text: String) {
        _state.update { it.copy(status = text) }
    }

    private companion object {
        val CONVERT_TO_JPEG = setOf("image/heic", "image/heif", "image/webp", "image/avif")
    }
}
