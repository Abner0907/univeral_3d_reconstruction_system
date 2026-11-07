package com.universal3d.capture

import android.Manifest
import android.content.pm.PackageManager
import android.os.Bundle
import android.util.Size
import android.view.Surface
import android.view.View
import android.widget.Toast
import androidx.activity.result.PickVisualMediaRequest
import androidx.activity.result.contract.ActivityResultContracts
import androidx.activity.viewModels
import androidx.appcompat.app.AppCompatActivity
import androidx.camera.core.CameraSelector
import androidx.camera.core.ImageCapture
import androidx.camera.core.ImageCaptureException
import androidx.camera.core.Preview
import androidx.camera.core.resolutionselector.AspectRatioStrategy
import androidx.camera.core.resolutionselector.ResolutionSelector
import androidx.camera.core.resolutionselector.ResolutionStrategy
import androidx.camera.lifecycle.ProcessCameraProvider
import androidx.core.content.ContextCompat
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.lifecycleScope
import androidx.lifecycle.repeatOnLifecycle
import com.universal3d.capture.databinding.ActivityMainBinding
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch

/**
 * Capture screen: CameraX preview, single-shot and timed auto capture, gallery picking, and
 * upload to the U3D reconstruction server (U3D Studio or u3d_server).
 */
class MainActivity : AppCompatActivity() {

    private lateinit var binding: ActivityMainBinding
    private val viewModel: CaptureViewModel by viewModels()
    private val prefs by lazy { getSharedPreferences("u3d", MODE_PRIVATE) }

    private var imageCapture: ImageCapture? = null
    private var captureInFlight = false
    private var autoCapture: Job? = null

    private val requestCameraPermission =
        registerForActivityResult(ActivityResultContracts.RequestPermission()) { granted ->
            if (granted) startCamera() else toast(getString(R.string.camera_permission_needed))
        }

    private val pickMedia =
        registerForActivityResult(ActivityResultContracts.PickMultipleVisualMedia()) { uris ->
            if (uris.isNotEmpty()) viewModel.addPicked(uris)
        }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        binding = ActivityMainBinding.inflate(layoutInflater)
        setContentView(binding.root)

        binding.serverUrl.setText(prefs.getString(KEY_URL, ""))
        binding.methodSpinner.setSelection(prefs.getInt(KEY_METHOD, 0))
        binding.iterations.setText(prefs.getInt(KEY_ITERATIONS, 30000).toString())

        binding.checkButton.setOnClickListener { viewModel.checkServer(serverUrl()) }
        binding.captureButton.setOnClickListener { takePhoto() }
        binding.autoButton.addOnCheckedChangeListener { _, checked -> setAutoCapture(checked) }
        binding.galleryButton.setOnClickListener {
            pickMedia.launch(PickVisualMediaRequest(ActivityResultContracts.PickVisualMedia.ImageAndVideo))
        }
        binding.clearButton.setOnClickListener { viewModel.clear() }
        binding.uploadButton.setOnClickListener { upload() }

        lifecycleScope.launch {
            repeatOnLifecycle(Lifecycle.State.STARTED) {
                viewModel.state.collect { render(it) }
            }
        }

        if (ContextCompat.checkSelfPermission(this, Manifest.permission.CAMERA) == PackageManager.PERMISSION_GRANTED) {
            startCamera()
        } else {
            requestCameraPermission.launch(Manifest.permission.CAMERA)
        }
    }

    override fun onPause() {
        super.onPause()
        binding.autoButton.isChecked = false
    }

    private fun serverUrl(): String = binding.serverUrl.text?.toString().orEmpty()

    private fun upload() {
        val methods = resources.getStringArray(R.array.method_values)
        val methodIndex = binding.methodSpinner.selectedItemPosition.coerceIn(0, methods.size - 1)
        val iterations = binding.iterations.text?.toString()?.toIntOrNull()?.coerceIn(100, 1_000_000) ?: 30000
        prefs.edit()
            .putString(KEY_URL, serverUrl())
            .putInt(KEY_METHOD, methodIndex)
            .putInt(KEY_ITERATIONS, iterations)
            .apply()
        binding.autoButton.isChecked = false
        viewModel.upload(serverUrl(), methods[methodIndex], iterations)
    }

    private fun render(state: UiState) {
        val parts = mutableListOf("${state.photoCount} photo(s)")
        if (state.pickedCount > 0) parts += "${state.pickedCount} from gallery"
        binding.counterText.text = parts.joinToString(" · ")

        binding.uploadButton.isEnabled = !state.busy && state.photoCount + state.pickedCount > 0
        binding.clearButton.isEnabled = !state.busy
        binding.galleryButton.isEnabled = !state.busy
        binding.statusText.text = state.status ?: getString(R.string.status_idle)
        binding.progressBar.visibility = if (state.progress == null) View.GONE else View.VISIBLE
        state.progress?.let { binding.progressBar.progress = it }
    }

    // ------------------------------------------------------------------------- camera

    private fun startCamera() {
        val providerFuture = ProcessCameraProvider.getInstance(this)
        providerFuture.addListener({
            val provider = providerFuture.get()
            val preview = Preview.Builder().build().also {
                it.setSurfaceProvider(binding.previewView.surfaceProvider)
            }
            // ~2.8 MP 4:3 frames: plenty for Nerfstudio (it downscales anyway) and quick to upload.
            val resolution = ResolutionSelector.Builder()
                .setAspectRatioStrategy(AspectRatioStrategy.RATIO_4_3_FALLBACK_AUTO_STRATEGY)
                .setResolutionStrategy(
                    ResolutionStrategy(Size(1920, 1440), ResolutionStrategy.FALLBACK_RULE_CLOSEST_LOWER_THEN_HIGHER)
                )
                .build()
            val capture = ImageCapture.Builder()
                .setCaptureMode(ImageCapture.CAPTURE_MODE_MINIMIZE_LATENCY)
                .setResolutionSelector(resolution)
                .setJpegQuality(95)
                .setTargetRotation(binding.previewView.display?.rotation ?: Surface.ROTATION_0)
                .build()
            try {
                provider.unbindAll()
                provider.bindToLifecycle(this, CameraSelector.DEFAULT_BACK_CAMERA, preview, capture)
                imageCapture = capture
            } catch (e: Exception) {
                toast("Camera unavailable: ${e.message}")
            }
        }, ContextCompat.getMainExecutor(this))
    }

    private fun takePhoto() {
        val capture = imageCapture ?: return toast(getString(R.string.camera_permission_needed))
        if (captureInFlight) return
        captureInFlight = true
        val file = viewModel.newPhotoFile()
        val options = ImageCapture.OutputFileOptions.Builder(file).build()
        capture.takePicture(options, ContextCompat.getMainExecutor(this), object : ImageCapture.OnImageSavedCallback {
            override fun onImageSaved(output: ImageCapture.OutputFileResults) {
                captureInFlight = false
                viewModel.onPhotoSaved(file)
                flashPreview()
            }

            override fun onError(exception: ImageCaptureException) {
                captureInFlight = false
                file.delete()
                toast("Capture failed: ${exception.message}")
            }
        })
    }

    /** Auto mode takes a photo every second while the user walks around the object. */
    private fun setAutoCapture(enabled: Boolean) {
        autoCapture?.cancel()
        autoCapture = null
        if (!enabled) return
        autoCapture = lifecycleScope.launch {
            while (isActive) {
                takePhoto()
                delay(AUTO_CAPTURE_INTERVAL_MS)
            }
        }
    }

    private fun flashPreview() {
        binding.previewView.animate().alpha(0.4f).setDuration(60).withEndAction {
            binding.previewView.animate().alpha(1f).setDuration(120).start()
        }.start()
    }

    private fun toast(text: String) {
        Toast.makeText(this, text, Toast.LENGTH_SHORT).show()
    }

    private companion object {
        const val KEY_URL = "server_url"
        const val KEY_METHOD = "method_index"
        const val KEY_ITERATIONS = "iterations"
        const val AUTO_CAPTURE_INTERVAL_MS = 1000L
    }
}
