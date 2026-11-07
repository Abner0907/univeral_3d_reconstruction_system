package com.universal3d.capture

import kotlin.math.roundToInt

/** Pure helpers (no Android APIs) so they can be unit-tested on the JVM. */
object ServerUrl {
    /**
     * Normalises what the user typed, e.g. "192.168.1.5:8000" -> "http://192.168.1.5:8000".
     * Returns null when the input cannot be a server URL.
     */
    fun normalize(input: String): String? {
        var s = input.trim()
        if (s.isEmpty()) return null
        if (!s.contains("://")) s = "http://$s"
        s = s.trimEnd('/')
        val scheme = s.substringBefore("://").lowercase()
        if (scheme != "http" && scheme != "https") return null
        val rest = s.substringAfter("://")
        val hostPort = rest.substringBefore('/')
        if (hostPort.isEmpty() || hostPort.any { it.isWhitespace() }) return null
        val host: String
        val port: String
        if (hostPort.startsWith("[")) { // IPv6 literal
            if (!hostPort.contains(']')) return null
            host = hostPort.substringBefore(']') + "]"
            port = hostPort.substringAfter(']').removePrefix(":")
        } else {
            host = hostPort.substringBefore(':')
            port = hostPort.substringAfter(':', "")
        }
        if (host.isEmpty() || host == "[]") return null
        if (port.isNotEmpty() && port.toIntOrNull()?.let { it in 1..65535 } != true) return null
        return "$scheme://$rest"
    }
}

object JobText {
    private val finished = setOf("succeeded", "failed", "cancelled")

    fun isFinished(status: String): Boolean = status in finished

    fun progressPermille(progress: Double): Int = (progress.coerceIn(0.0, 1.0) * 1000).roundToInt()

    /** One-line status shown under the buttons, e.g. "Training splatfacto: 41.0% (52%)". */
    fun describe(status: String, message: String, progress: Double): String {
        val percent = (progress.coerceIn(0.0, 1.0) * 100).roundToInt()
        return when (status) {
            "succeeded" -> "Done! Open the result in U3D Studio on your computer."
            "failed" -> "Failed: $message"
            "cancelled" -> "Cancelled."
            "queued" -> "Waiting in queue on the server…"
            else -> if (message.isBlank()) "Working… ($percent%)" else "$message ($percent%)"
        }
    }
}
