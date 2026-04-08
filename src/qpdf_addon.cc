#include <napi.h>

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
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
  auto closer = [](FILE *fp) {
    if (fp)
      fclose(fp);
  };
  std::unique_ptr<FILE, decltype(closer)> f(fopen(path.c_str(), "wb"), closer);
  if (!f) {
    auto parentDir = std::filesystem::path(path).parent_path();
    if (!parentDir.empty() && !std::filesystem::is_directory(parentDir))
      return "Parent directory does not exist: " + parentDir.string();
    return "Failed to open output file: " + path + " (" +
           std::string(std::strerror(errno)) + ")";
  }
  if (fwrite(data, 1, size, f.get()) != size)
    return "Failed to write output file: " + path + " (" +
           std::string(std::strerror(errno)) + ")";
  if (fflush(f.get()) != 0)
    return "Failed to flush output file: " + path + " (" +
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

#define CHECK_ENV()                                                            \
  if (envAlive_ && !envAlive_->load())                                         \
    return;

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

protected:
  void Execute() override {
    try {
      QPDF qpdf;
      qpdf.setAttemptRecovery(true);
      qpdf.setSuppressWarnings(true);

      if (useFile_) {
        if (!std::filesystem::exists(filePath_)) {
          SetError("Input file not found: " + filePath_);
          return;
        }
        // validate PDF header to prevent QPDF from aborting on garbage input
        {
          auto closer = [](FILE *fp) {
            if (fp)
              fclose(fp);
          };
          std::unique_ptr<FILE, decltype(closer)> f(
              fopen(filePath_.c_str(), "rb"), closer);
          if (f) {
            char hdr[5] = {};
            if (fread(hdr, 1, 5, f.get()) < 5 || memcmp(hdr, "%PDF-", 5) != 0) {
              SetError("Input is not a valid PDF (missing %PDF- header)");
              return;
            }
          }
        }
        qpdf.processFile(filePath_.c_str());
      } else {
        // validate PDF header to prevent QPDF from aborting on garbage input
        if (bufferData_.size() < 5 ||
            memcmp(bufferData_.data(), "%PDF-", 5) != 0) {
          SetError("Input is not a valid PDF (missing %PDF- header)");
          return;
        }
        qpdf.processMemoryFile(
            "input.pdf", reinterpret_cast<const char *>(bufferData_.data()),
            bufferData_.size());
      }

      deduplicateImages(qpdf);
      convertGrayscaleImages(qpdf);
      convertBitonalImages(qpdf);
      flattenPageTree(qpdf);
      stripDocumentOverhead(qpdf);

      if (lossy_) {
        // lossy: re-encode high-quality JPEGs at lower quality
        CompressOptions opts;
        opts.skipThreshold = 65;
        opts.targetQuality = 75;
        optimizeImages(qpdf, opts);
        downscaleImages(qpdf, 72, 75);
      }

      // lossless Huffman optimization for all existing JPEGs
      optimizeExistingJpegs(qpdf);
      optimizeSoftMasks(qpdf);
      removeUnusedFonts(qpdf);
      subsetFonts(qpdf);
      stripIccProfiles(qpdf);
      flattenForms(qpdf);
      removeUnusedResources(qpdf);
      coalesceContentStreams(qpdf);
      minifyContentStreams(qpdf);
      deduplicateStreams(qpdf);
      stripEmbeddedFiles(qpdf);
      stripJavaScript(qpdf);
      if (stripMeta_)
        stripMetadata(qpdf);

      Pl_Flate::setCompressionLevel(9);

      QPDFWriter writer(qpdf);
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

      auto buf = writer.getBufferSharedPointer();
      result_.assign(buf->getBuffer(), buf->getBuffer() + buf->getSize());

      if (!outputPath_.empty()) {
        auto err = writeToFile(outputPath_, result_.data(), result_.size());
        if (!err.empty()) {
          SetError(err);
          return;
        }
        result_.clear();
      }
    } catch (std::exception &e) {
      SetError(e.what());
    }
  }

  void OnOK() override {
    CHECK_ENV();
    if (outputPath_.empty()) {
      deferred_.Resolve(
          Napi::Buffer<uint8_t>::Copy(Env(), result_.data(), result_.size()));
    } else {
      deferred_.Resolve(Env().Undefined());
    }
  }

  void OnError(Napi::Error const &error) override {
    CHECK_ENV();
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
  std::vector<uint8_t> result_;
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
  env.AddCleanupHook([envAlive]() { envAlive->store(false); });

  exports.Set("compress", Napi::Function::New(env, Compress));
  return exports;
}

NODE_API_MODULE(qpdf_compress, Init)
