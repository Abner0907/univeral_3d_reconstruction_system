package com.universal3d.capture

import okhttp3.HttpUrl
import okhttp3.HttpUrl.Companion.toHttpUrl
import okhttp3.MediaType.Companion.toMediaType
import okhttp3.OkHttpClient
import okhttp3.Request
import okhttp3.RequestBody.Companion.asRequestBody
import okhttp3.RequestBody.Companion.toRequestBody
import org.json.JSONObject
import java.io.File
import java.io.IOException
import java.util.concurrent.TimeUnit

/** A reconstruction job as reported by the server. */
data class RemoteJob(
    val id: String,
    val name: String,
    val status: String,
    val stage: String,
    val progress: Double,
    val message: String,
    val numInputs: Int,
) {
    val isFinished: Boolean get() = JobText.isFinished(status)

    companion object {
        fun fromJson(o: JSONObject) = RemoteJob(
            id = o.optString("id"),
            name = o.optString("name"),
            status = o.optString("status"),
            stage = o.optString("stage"),
            progress = o.optDouble("progress", 0.0),
            message = o.optString("message"),
            numInputs = o.optInt("num_inputs"),
        )
    }
}

class ReconApiException(message: String) : IOException(message)

/**
 * Client for the REST API served by U3D Studio / u3d_server.
 * Upload flow: [createJob] -> [uploadFile] per image -> [startJob] -> poll [job].
 * All calls block; run them on Dispatchers.IO.
 */
class ReconApi(baseUrl: String, private val client: OkHttpClient = defaultClient) {
    private val base: HttpUrl = baseUrl.trimEnd('/').toHttpUrl()

    fun info(): JSONObject = execute(Request.Builder().url(url("api", "info")).get().build())

    fun createJob(name: String, method: String, iterations: Int): RemoteJob {
        val body = JSONObject()
            .put("name", name)
            .put("method", method)
            .put("iterations", iterations)
            .toString()
            .toRequestBody(JSON)
        return RemoteJob.fromJson(execute(Request.Builder().url(url("api", "jobs")).post(body).build()))
    }

    fun uploadFile(jobId: String, fileName: String, file: File, mimeType: String) {
        // A file body has a known length, so OkHttp sends Content-Length (the server
        // does not accept chunked uploads).
        val request = Request.Builder()
            .url(url("api", "jobs", jobId, "files", fileName))
            .put(file.asRequestBody(mimeType.toMediaType()))
            .build()
        execute(request)
    }

    fun startJob(jobId: String): RemoteJob = RemoteJob.fromJson(
        execute(Request.Builder().url(url("api", "jobs", jobId, "start")).post(ByteArray(0).toRequestBody(JSON)).build())
    )

    fun job(jobId: String): RemoteJob =
        RemoteJob.fromJson(execute(Request.Builder().url(url("api", "jobs", jobId)).get().build()))

    private fun url(vararg segments: String): HttpUrl {
        val builder = base.newBuilder()
        segments.forEach { builder.addPathSegment(it) }
        return builder.build()
    }

    private fun execute(request: Request): JSONObject {
        client.newCall(request).execute().use { response ->
            val text = response.body?.string().orEmpty()
            val json = runCatching { JSONObject(text) }.getOrNull()
            if (!response.isSuccessful) {
                val error = json?.optString("error")?.takeIf { it.isNotBlank() } ?: "HTTP ${response.code}"
                throw ReconApiException(error)
            }
            return json ?: JSONObject()
        }
    }

    companion object {
        private val JSON = "application/json; charset=utf-8".toMediaType()

        val defaultClient: OkHttpClient = OkHttpClient.Builder()
            .connectTimeout(5, TimeUnit.SECONDS)
            .writeTimeout(2, TimeUnit.MINUTES)
            .readTimeout(1, TimeUnit.MINUTES)
            .build()
    }
}
