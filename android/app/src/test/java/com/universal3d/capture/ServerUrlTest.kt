package com.universal3d.capture

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class ServerUrlTest {
    @Test
    fun addsSchemeAndTrimsSlash() {
        assertEquals("http://192.168.1.20:8000", ServerUrl.normalize(" 192.168.1.20:8000/ "))
        assertEquals("https://recon.example.com", ServerUrl.normalize("https://recon.example.com"))
        assertEquals("http://[fe80::1]:8000", ServerUrl.normalize("[fe80::1]:8000"))
        assertEquals("http://host/base", ServerUrl.normalize("host/base/"))
    }

    @Test
    fun rejectsInvalidInput() {
        assertNull(ServerUrl.normalize(""))
        assertNull(ServerUrl.normalize("ftp://host"))
        assertNull(ServerUrl.normalize("http://"))
        assertNull(ServerUrl.normalize("host:99999"))
        assertNull(ServerUrl.normalize("host:abc"))
        assertNull(ServerUrl.normalize("my host:8000"))
    }

    @Test
    fun describesJobs() {
        assertTrue(JobText.isFinished("succeeded"))
        assertFalse(JobText.isFinished("running"))
        assertEquals(415, JobText.progressPermille(0.4151))
        assertEquals(1000, JobText.progressPermille(1.7))
        assertEquals("Training splatfacto: 40.0% (49%)", JobText.describe("running", "Training splatfacto: 40.0%", 0.49))
        assertEquals("Failed: boom", JobText.describe("failed", "boom", 0.3))
    }
}
