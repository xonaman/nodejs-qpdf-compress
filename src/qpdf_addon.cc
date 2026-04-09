#include <napi.h>

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#ifdef _WIN32
#include <io.h>
#define F_OK 0
#define access _access
#else
#include <unistd.h>
#endif
#include <vector>

#include <qpdf/Pl_Flate.hh>
#include <qpdf/QPDF.hh>
#include <qpdf/QPDFWriter.hh>

#include "images.h"
#include "optimize.h"

// ---------------------------------------------------------------------------
// File output helper — returns empty string on success, error message on
// failure
// ---------------------------------------------------------------------------

static std::string writeToFile(const std::string &path, const uint8_t *data,
                               size_t size) {
  FILE *f = fopen(path.c_str(), "wb");
  if (!f) {
    auto slash = path.rfind('/');
    if (slash != std::string::npos) {
      auto parentDir = path.substr(0, slash);
      if (access(parentDir.c_str(), F_OK) != 0)
        return "Parent directory does not exist: " + parentDir;
    }
    return "Failed to open output file: " + path + " (" +
           std::string(std::strerror(errno)) + ")";
  }
  size_t written = fwrite(data, 1, size, f);
  int closeErr = fclose(f);
  if (written != size)
    return "Failed to write output file: " + path + " (" +
           std::string(std::strerror(errno)) + ")";
  if (closeErr != 0)
    return "Failed to close output file: " + path + " (" +
           std::string(std::strerror(errno)) + ")";
  return {};
}

// ---------------------------------------------------------------------------
// Per-environment alive flag for worker thread safety.
// When a Node.js worker thread is terminated, the V8 isolate tears down but
// libuv async callbacks (OnOK/OnError) may still fire. This flag lets workers
// bail out before touching V8 handles on a torn-down isolate.
// ---------------------------------------------------------------------------

struct AddonData {
  std::shared_ptr<std::atomic<bool>> envAlive =
      std::make_shared<std::atomic<bool>>(true);
};

static std::shared_ptr<std::atomic<bool>> GetEnvAlive(Napi::Env env) {
  auto *data = env.GetInstanceData<AddonData>();
  return data ? data->envAlive : nullptr;
}

// ---------------------------------------------------------------------------
// CompressWorker — async PDF compression
// ---------------------------------------------------------------------------

class CompressWorker : public Napi::AsyncWorker {
public:
  // buffer variant
  CompressWorker(Napi::Env env, std::vector<uint8_t> data, bool lossy,
                 bool stripMeta, std::string outputPath)
      : Napi::AsyncWorker(env), deferred_(Napi::Promise::Deferred::New(env)),
        envAlive_(GetEnvAlive(env)), bufferData_(std::move(data)),
        lossy_(lossy), stripMeta_(stripMeta), useFile_(false),
        outputPath_(std::move(outputPath)) {}

  // file path variant
  CompressWorker(Napi::Env env, std::string path, bool lossy, bool stripMeta,
                 std::string outputPath)
      : Napi::AsyncWorker(env), deferred_(Napi::Promise::Deferred::New(env)),
        envAlive_(GetEnvAlive(env)), filePath_(std::move(path)), lossy_(lossy),
        stripMeta_(stripMeta), useFile_(true),
        outputPath_(std::move(outputPath)) {}

  Napi::Promise Promise() { return deferred_.Promise(); }

  // guard against worker-thread teardown: if the env is being destroyed
  // (e.g. worker.terminate()), skip all V8 API calls — the JS promise will
  // never settle, but there is no listener for the result anyway.
  void OnWorkComplete(Napi::Env env, napi_status status) override {
    if (envAlive_ && !envAlive_->load())
      return;

    // probe whether V8 is still accessible (raw C call, no throw on failure)
    napi_handle_scope scope = nullptr;
    if (napi_open_handle_scope(env, &scope) != napi_ok)
      return;
    napi_close_handle_scope(env, scope);

    try {
      Napi::AsyncWorker::OnWorkComplete(env, status);
    } catch (const Napi::Error &) {
      // env tore down between probe and base class call
    }
  }

protected:
  void Execute() override {
    // heap-allocate QPDF so we control destruction order — if processFile/
    // processMemoryFile throws, the QPDF destructor may also throw (broken
    // internal state), which during stack unwinding would call std::terminate.
    auto qpdf = std::make_unique<QPDF>();
    try {
      qpdf->setAttemptRecovery(true);
      qpdf->setSuppressWarnings(true);

      if (useFile_) {
        if (access(filePath_.c_str(), F_OK) != 0) {
          SetError("Input file not found: " + filePath_);
          return;
        }
        qpdf->processFile(filePath_.c_str());
      } else {
        qpdf->processMemoryFile(
            "input.pdf", reinterpret_cast<const char *>(bufferData_.data()),
            bufferData_.size());
      }

      deduplicateImages(*qpdf);
      convertGrayscaleImages(*qpdf);
      convertBitonalImages(*qpdf);
      flattenPageTree(*qpdf);
      stripDocumentOverhead(*qpdf);

      if (lossy_) {
        // lossy: re-encode high-quality JPEGs at lower quality
        CompressOptions opts;
        opts.skipThreshold = 65;
        opts.targetQuality = 75;
        optimizeImages(*qpdf, opts);
        downscaleImages(*qpdf, 72, 75);
      }

      // lossless Huffman optimization for all existing JPEGs
      optimizeExistingJpegs(*qpdf);
      optimizeSoftMasks(*qpdf);
      removeUnusedFonts(*qpdf);
      subsetFonts(*qpdf);
      stripIccProfiles(*qpdf);
      flattenForms(*qpdf);
      removeUnusedResources(*qpdf);
      coalesceContentStreams(*qpdf);
      minifyContentStreams(*qpdf);
      deduplicateStreams(*qpdf);
      stripEmbeddedFiles(*qpdf);
      stripJavaScript(*qpdf);
      if (stripMeta_)
        stripMetadata(*qpdf);

      Pl_Flate::setCompressionLevel(9);

      QPDFWriter writer(*qpdf);
      writer.setOutputMemory();
      writer.setLinearization(false);
      writer.setStreamDataMode(qpdf_s_compress);
      writer.setRecompressFlate(true);
      writer.setObjectStreamMode(qpdf_o_generate);
      writer.setCompressStreams(true);
      // only decode generalized streams (Flate, LZW, etc.)
      // this preserves DCTDecode (our recompressed JPEG images)
      writer.setDecodeLevel(qpdf_dl_generalized);
      writer.setPreserveUnreferencedObjects(false);
      writer.write();

      writerBuf_ = writer.getBufferSharedPointer();

      // release QPDF before file I/O — no longer needed
      qpdf.reset();

      if (!outputPath_.empty()) {
        auto err = writeToFile(outputPath_, writerBuf_->getBuffer(),
                               writerBuf_->getSize());
        if (!err.empty()) {
          SetError(err);
          return;
        }
        writerBuf_.reset();
      }
    } catch (std::exception &e) {
      // destroy QPDF inside the catch so its destructor can't cause
      // std::terminate via a double exception during stack unwinding
      try {
        qpdf.reset();
      } catch (...) {
      }
      SetError(e.what());
    } catch (...) {
      // on macOS/libc++, catch(std::exception&) may fail to match QPDFExc
      // due to typeinfo visibility mismatch between the static library and
      // our addon compiled with -fvisibility=hidden
      try {
        qpdf.reset();
      } catch (...) {
      }
      SetError("PDF compression failed");
    }
  }

  void OnOK() override {
    if (outputPath_.empty()) {
      // zero-copy: transfer QPDFWriter's buffer directly to JS, preventing
      // a redundant full-copy of the compressed PDF
      auto *prevent_copy = new std::shared_ptr<Buffer>(std::move(writerBuf_));
      deferred_.Resolve(Napi::Buffer<uint8_t>::New(
          Env(), prevent_copy->get()->getBuffer(),
          prevent_copy->get()->getSize(),
          [](Napi::Env, uint8_t *, std::shared_ptr<Buffer> *p) { delete p; },
          prevent_copy));
    } else {
      deferred_.Resolve(Env().Undefined());
    }
  }

  void OnError(Napi::Error const &error) override {
    deferred_.Reject(error.Value());
  }

private:
  Napi::Promise::Deferred deferred_;
  std::shared_ptr<std::atomic<bool>> envAlive_;
  std::vector<uint8_t> bufferData_;
  std::string filePath_;
  bool lossy_;
  bool stripMeta_;
  bool useFile_;
  std::string outputPath_;
  std::shared_ptr<Buffer> writerBuf_;
};

// ---------------------------------------------------------------------------
// JS API: compress(input, options)
// ---------------------------------------------------------------------------

static Napi::Value Compress(const Napi::CallbackInfo &info) {
  Napi::Env env = info.Env();

  if (info.Length() < 1) {
    Napi::TypeError::New(env, "Expected input (Buffer or string)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  bool lossy = false;
  bool stripMeta = false;
  std::string outputPath;

  if (info.Length() >= 2 && info[1].IsObject()) {
    auto options = info[1].As<Napi::Object>();

    if (options.Has("lossy"))
      lossy = options.Get("lossy").As<Napi::Boolean>().Value();

    if (options.Has("stripMetadata"))
      stripMeta = options.Get("stripMetadata").As<Napi::Boolean>().Value();

    if (options.Has("output"))
      outputPath = options.Get("output").As<Napi::String>().Utf8Value();
  }

  if (info[0].IsBuffer()) {
    auto buf = info[0].As<Napi::Buffer<uint8_t>>();
    std::vector<uint8_t> data(buf.Data(), buf.Data() + buf.Length());
    auto *worker = new CompressWorker(env, std::move(data), lossy, stripMeta,
                                      std::move(outputPath));
    worker->Queue();
    return worker->Promise();
  }

  if (info[0].IsString()) {
    auto path = info[0].As<Napi::String>().Utf8Value();
    auto *worker = new CompressWorker(env, std::move(path), lossy, stripMeta,
                                      std::move(outputPath));
    worker->Queue();
    return worker->Promise();
  }

  Napi::TypeError::New(env, "Input must be a Buffer or file path string")
      .ThrowAsJavaScriptException();
  return env.Undefined();
}

// ---------------------------------------------------------------------------
// Module init
// ---------------------------------------------------------------------------

static Napi::Object Init(Napi::Env env, Napi::Object exports) {
  auto *addonData = new AddonData();
  env.SetInstanceData(addonData);
  auto envAlive = addonData->envAlive;
  env.AddCleanupHook([addonData]() {
    addonData->envAlive->store(false);
    delete addonData;
  });

  exports.Set("compress", Napi::Function::New(env, Compress));
  return exports;
}

NODE_API_MODULE(qpdf_compress, Init)
