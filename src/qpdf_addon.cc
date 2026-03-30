#include <napi.h>

#include <cerrno>
#include <csetjmp>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include <jpeglib.h>

#include <qpdf/Buffer.hh>
#include <qpdf/Pl_Flate.hh>
#include <qpdf/QPDF.hh>
#include <qpdf/QPDFObjectHandle.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>
#include <qpdf/QPDFWriter.hh>

#include "stb_image_write.h"

static std::mutex g_qpdf_mutex;

// ---------------------------------------------------------------------------
// stb_image_write callback — writes JPEG data to a vector
// ---------------------------------------------------------------------------

static void stbi_write_to_vector(void *context, void *data, int size) {
  if (!context || !data || size <= 0)
    return;
  auto *vec = static_cast<std::vector<uint8_t> *>(context);
  auto *bytes = static_cast<uint8_t *>(data);
  vec->insert(vec->end(), bytes, bytes + size);
}

// ---------------------------------------------------------------------------
// JPEG error handler — prevents libjpeg from calling exit() on errors
// ---------------------------------------------------------------------------

struct JpegErrorMgr {
  struct jpeg_error_mgr pub;
  std::jmp_buf jmpbuf;
};

static void jpegErrorExit(j_common_ptr cinfo) {
  auto *myerr = reinterpret_cast<JpegErrorMgr *>(cinfo->err);
  std::longjmp(myerr->jmpbuf, 1);
}

// ---------------------------------------------------------------------------
// Lossless JPEG optimization — rewrites Huffman tables at the DCT coefficient
// level without touching pixel data. Typically saves 2–15%.
// ---------------------------------------------------------------------------

static bool losslessJpegOptimize(const unsigned char *data, size_t size,
                                 std::vector<uint8_t> &out) {
  struct jpeg_decompress_struct srcinfo = {};
  struct jpeg_compress_struct dstinfo = {};
  JpegErrorMgr jerr = {};
  unsigned char *outbuf = nullptr;

  srcinfo.err = jpeg_std_error(&jerr.pub);
  jerr.pub.error_exit = jpegErrorExit;
  dstinfo.err = &jerr.pub;

  jpeg_create_decompress(&srcinfo);
  jpeg_create_compress(&dstinfo);

  if (setjmp(jerr.jmpbuf)) {
    jpeg_destroy_decompress(&srcinfo);
    jpeg_destroy_compress(&dstinfo);
    free(outbuf);
    return false;
  }

  jpeg_mem_src(&srcinfo, data, static_cast<unsigned long>(size));

  if (jpeg_read_header(&srcinfo, TRUE) != JPEG_HEADER_OK) {
    jpeg_destroy_decompress(&srcinfo);
    jpeg_destroy_compress(&dstinfo);
    return false;
  }

  // read DCT coefficients — zero quality loss
  jvirt_barray_ptr *coef_arrays = jpeg_read_coefficients(&srcinfo);
  if (!coef_arrays) {
    jpeg_destroy_decompress(&srcinfo);
    jpeg_destroy_compress(&dstinfo);
    return false;
  }

  unsigned long outsize = 0;
  jpeg_mem_dest(&dstinfo, &outbuf, &outsize);

  jpeg_copy_critical_parameters(&srcinfo, &dstinfo);
  dstinfo.optimize_coding = TRUE;

  jpeg_write_coefficients(&dstinfo, coef_arrays);
  jpeg_finish_compress(&dstinfo);
  jpeg_finish_decompress(&srcinfo);

  if (outbuf && outsize > 0) {
    out.assign(outbuf, outbuf + outsize);
  }

  free(outbuf);
  jpeg_destroy_compress(&dstinfo);
  jpeg_destroy_decompress(&srcinfo);

  return !out.empty();
}

// ---------------------------------------------------------------------------
// Image recompression for lossy mode
// ---------------------------------------------------------------------------

static void optimizeImages(QPDF &qpdf, int quality) {
  for (auto &page : QPDFPageDocumentHelper(qpdf).getAllPages()) {
    auto pageObj = page.getObjectHandle();
    auto resources = pageObj.getKey("/Resources");
    if (!resources.isDictionary())
      continue;
    auto xobjects = resources.getKey("/XObject");
    if (!xobjects.isDictionary())
      continue;

    for (auto &key : xobjects.getKeys()) {
      auto xobj = xobjects.getKey(key);
      if (!xobj.isStream())
        continue;

      auto dict = xobj.getDict();
      if (!dict.getKey("/Subtype").isName() ||
          dict.getKey("/Subtype").getName() != "/Image")
        continue;

      // only handle 8-bit images
      if (!dict.getKey("/BitsPerComponent").isInteger() ||
          dict.getKey("/BitsPerComponent").getIntValue() != 8)
        continue;

      int width = 0, height = 0, components = 0;
      if (dict.getKey("/Width").isInteger())
        width = static_cast<int>(dict.getKey("/Width").getIntValue());
      if (dict.getKey("/Height").isInteger())
        height = static_cast<int>(dict.getKey("/Height").getIntValue());

      if (width <= 0 || height <= 0 || width > 65536 || height > 65536)
        continue;

      // determine color components
      auto cs = dict.getKey("/ColorSpace");
      if (cs.isName()) {
        if (cs.getName() == "/DeviceRGB")
          components = 3;
        else if (cs.getName() == "/DeviceGray")
          components = 1;
        else
          continue; // skip CMYK, Lab, etc. for now
      } else {
        continue; // skip indexed, ICCBased, etc.
      }

      // skip tiny images (logos, icons) — not worth recompressing
      if (width * height < 2500)
        continue;

      // get fully decoded stream data (raw pixels)
      std::shared_ptr<Buffer> streamData;
      try {
        streamData = xobj.getStreamData(qpdf_dl_all);
      } catch (...) {
        continue; // can't decode — skip
      }

      // overflow-safe size calculation
      auto w = static_cast<size_t>(width);
      auto h = static_cast<size_t>(height);
      auto c = static_cast<size_t>(components);
      if (h > 0 && w > std::numeric_limits<size_t>::max() / h)
        continue;
      if (c > 0 && (w * h) > std::numeric_limits<size_t>::max() / c)
        continue;
      size_t expectedSize = w * h * c;
      if (streamData->getSize() != expectedSize)
        continue;

      // check if recompression would actually help:
      // skip if already a small JPEG
      auto currentFilter = dict.getKey("/Filter");
      bool isCurrentlyJpeg =
          currentFilter.isName() && currentFilter.getName() == "/DCTDecode";

      // encode as JPEG
      std::vector<uint8_t> jpegData;
      jpegData.reserve(expectedSize / 4); // estimate
      int writeOk =
          stbi_write_jpg_to_func(stbi_write_to_vector, &jpegData, width, height,
                                 components, streamData->getBuffer(), quality);

      if (!writeOk || jpegData.empty())
        continue;

      // only replace if we actually reduced size
      if (isCurrentlyJpeg) {
        auto rawData = xobj.getRawStreamData();
        if (jpegData.size() >= rawData->getSize())
          continue; // new JPEG is larger, keep original
      }

      // replace stream data with JPEG
      std::string jpegStr(reinterpret_cast<char *>(jpegData.data()),
                          jpegData.size());
      xobj.replaceStreamData(jpegStr, QPDFObjectHandle::newName("/DCTDecode"),
                             QPDFObjectHandle::newNull());

      // update dictionary — remove FlateDecode-specific params
      if (dict.hasKey("/DecodeParms"))
        dict.removeKey("/DecodeParms");
      if (dict.hasKey("/Predictor"))
        dict.removeKey("/Predictor");
    }
  }
}

// ---------------------------------------------------------------------------
// Duplicate image detection — replaces identical image objects with
// references to a single canonical copy. Dropped duplicates become
// unreferenced and are omitted from the output.
// ---------------------------------------------------------------------------

static void deduplicateImages(QPDF &qpdf) {
  struct ImageEntry {
    QPDFObjGen og;
    size_t dataSize;
    QPDFObjectHandle handle;
  };

  std::unordered_map<size_t, std::vector<ImageEntry>> hashGroups;
  std::set<QPDFObjGen> seen;

  // first pass: collect all image objects and hash their raw data
  for (auto &page : QPDFPageDocumentHelper(qpdf).getAllPages()) {
    auto resources = page.getObjectHandle().getKey("/Resources");
    if (!resources.isDictionary())
      continue;
    auto xobjects = resources.getKey("/XObject");
    if (!xobjects.isDictionary())
      continue;

    for (auto &key : xobjects.getKeys()) {
      auto xobj = xobjects.getKey(key);
      if (!xobj.isStream())
        continue;
      auto og = xobj.getObjGen();
      if (seen.count(og))
        continue;
      seen.insert(og);

      auto dict = xobj.getDict();
      if (!dict.getKey("/Subtype").isName() ||
          dict.getKey("/Subtype").getName() != "/Image")
        continue;

      try {
        auto rawData = xobj.getRawStreamData();
        size_t size = rawData->getSize();

        // FNV-1a hash
        size_t hash = 14695981039346656037ULL;
        auto *p = rawData->getBuffer();
        for (size_t i = 0; i < size; ++i) {
          hash ^= static_cast<size_t>(p[i]);
          hash *= 1099511628211ULL;
        }

        hashGroups[hash].push_back({og, size, xobj});
      } catch (...) {
        continue;
      }
    }
  }

  // second pass: verify hash collisions with full byte comparison
  std::map<QPDFObjGen, QPDFObjectHandle> replacements;

  for (auto &[hash, group] : hashGroups) {
    if (group.size() < 2)
      continue;

    for (size_t i = 0; i < group.size(); ++i) {
      if (replacements.count(group[i].og))
        continue;

      auto rawI = group[i].handle.getRawStreamData();
      for (size_t j = i + 1; j < group.size(); ++j) {
        if (replacements.count(group[j].og))
          continue;

        auto rawJ = group[j].handle.getRawStreamData();
        if (rawI->getSize() != rawJ->getSize())
          continue;

        if (memcmp(rawI->getBuffer(), rawJ->getBuffer(), rawI->getSize()) ==
            0) {
          replacements[group[j].og] = group[i].handle;
        }
      }
    }
  }

  if (replacements.empty())
    return;

  // third pass: rewrite XObject references to point to canonical objects
  for (auto &page : QPDFPageDocumentHelper(qpdf).getAllPages()) {
    auto resources = page.getObjectHandle().getKey("/Resources");
    if (!resources.isDictionary())
      continue;
    auto xobjects = resources.getKey("/XObject");
    if (!xobjects.isDictionary())
      continue;

    for (auto &key : xobjects.getKeys()) {
      auto xobj = xobjects.getKey(key);
      auto it = replacements.find(xobj.getObjGen());
      if (it != replacements.end()) {
        xobjects.replaceKey(key, it->second);
      }
    }
  }
}

// ---------------------------------------------------------------------------
// Lossless optimization of existing embedded JPEG images — optimizes Huffman
// tables at the DCT coefficient level without any quality loss.
// ---------------------------------------------------------------------------

static void optimizeExistingJpegs(QPDF &qpdf) {
  std::set<QPDFObjGen> processed;

  for (auto &page : QPDFPageDocumentHelper(qpdf).getAllPages()) {
    auto resources = page.getObjectHandle().getKey("/Resources");
    if (!resources.isDictionary())
      continue;
    auto xobjects = resources.getKey("/XObject");
    if (!xobjects.isDictionary())
      continue;

    for (auto &key : xobjects.getKeys()) {
      auto xobj = xobjects.getKey(key);
      if (!xobj.isStream())
        continue;

      auto og = xobj.getObjGen();
      if (processed.count(og))
        continue;
      processed.insert(og);

      auto dict = xobj.getDict();
      auto filter = dict.getKey("/Filter");
      if (!filter.isName() || filter.getName() != "/DCTDecode")
        continue;

      try {
        auto rawData = xobj.getRawStreamData();

        std::vector<uint8_t> optimized;
        if (!losslessJpegOptimize(rawData->getBuffer(), rawData->getSize(),
                                  optimized))
          continue;

        // only replace if strictly smaller
        if (optimized.size() >= rawData->getSize())
          continue;

        std::string jpegStr(reinterpret_cast<char *>(optimized.data()),
                            optimized.size());
        xobj.replaceStreamData(jpegStr, QPDFObjectHandle::newName("/DCTDecode"),
                               QPDFObjectHandle::newNull());
      } catch (...) {
        continue;
      }
    }
  }
}

// ---------------------------------------------------------------------------
// CompressWorker — async PDF compression
// ---------------------------------------------------------------------------

class CompressWorker : public Napi::AsyncWorker {
public:
  // buffer variant
  CompressWorker(Napi::Env env, std::vector<uint8_t> data, bool lossy,
                 int quality, std::string outputPath)
      : Napi::AsyncWorker(env), deferred_(Napi::Promise::Deferred::New(env)),
        bufferData_(std::move(data)), lossy_(lossy), quality_(quality),
        useFile_(false), outputPath_(std::move(outputPath)) {}

  // file path variant
  CompressWorker(Napi::Env env, std::string path, bool lossy, int quality,
                 std::string outputPath)
      : Napi::AsyncWorker(env), deferred_(Napi::Promise::Deferred::New(env)),
        filePath_(std::move(path)), lossy_(lossy), quality_(quality),
        useFile_(true), outputPath_(std::move(outputPath)) {}

  Napi::Promise Promise() { return deferred_.Promise(); }

protected:
  void Execute() override {
    try {
      std::lock_guard<std::mutex> lock(g_qpdf_mutex);

      QPDF qpdf;
      qpdf.setAttemptRecovery(true);

      if (useFile_) {
        if (!std::filesystem::exists(filePath_)) {
          SetError("Input file not found: " + filePath_);
          return;
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

      // deduplicate identical images across pages
      deduplicateImages(qpdf);

      // lossy: recompress embedded images as JPEG
      if (lossy_) {
        optimizeImages(qpdf, quality_);
      }

      // lossless JPEG Huffman table optimization
      optimizeExistingJpegs(qpdf);

      // maximum Flate compression level
      Pl_Flate::setCompressionLevel(9);

      QPDFWriter writer(qpdf);
      writer.setOutputMemory();
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

      // write to file if output path was specified
      if (!outputPath_.empty()) {
        auto closer = [](FILE *fp) {
          if (fp)
            fclose(fp);
        };
        std::unique_ptr<FILE, decltype(closer)> f(
            fopen(outputPath_.c_str(), "wb"), closer);
        if (!f) {
          auto parentDir = std::filesystem::path(outputPath_).parent_path();
          if (!parentDir.empty() && !std::filesystem::is_directory(parentDir)) {
            SetError("Parent directory does not exist: " + parentDir.string());
          } else {
            SetError("Failed to open output file: " + outputPath_ + " (" +
                     std::strerror(errno) + ")");
          }
          return;
        }
        size_t written = fwrite(result_.data(), 1, result_.size(), f.get());
        if (written != result_.size()) {
          SetError("Failed to write output file: " + outputPath_ + " (" +
                   std::strerror(errno) + ")");
          return;
        }
        if (fflush(f.get()) != 0) {
          SetError("Failed to flush output file: " + outputPath_ + " (" +
                   std::strerror(errno) + ")");
          return;
        }
        result_.clear();
      }
    } catch (std::exception &e) {
      SetError(e.what());
    }
  }

  void OnOK() override {
    if (outputPath_.empty()) {
      auto buffer =
          Napi::Buffer<uint8_t>::Copy(Env(), result_.data(), result_.size());
      deferred_.Resolve(buffer);
    } else {
      deferred_.Resolve(Env().Undefined());
    }
  }

  void OnError(Napi::Error const &error) override {
    deferred_.Reject(error.Value());
  }

private:
  Napi::Promise::Deferred deferred_;
  std::vector<uint8_t> bufferData_;
  std::string filePath_;
  bool lossy_;
  int quality_;
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

  // parse options
  bool lossy = false;
  int quality = 75;
  std::string outputPath;

  if (info.Length() >= 2 && info[1].IsObject()) {
    auto options = info[1].As<Napi::Object>();

    if (options.Has("mode")) {
      auto mode = options.Get("mode").As<Napi::String>().Utf8Value();
      lossy = (mode == "lossy");
    }

    if (options.Has("quality")) {
      quality = options.Get("quality").As<Napi::Number>().Int32Value();
      if (quality < 1)
        quality = 1;
      if (quality > 100)
        quality = 100;
    }

    if (options.Has("output"))
      outputPath = options.Get("output").As<Napi::String>().Utf8Value();
  }

  if (info[0].IsBuffer()) {
    auto buf = info[0].As<Napi::Buffer<uint8_t>>();
    std::vector<uint8_t> data(buf.Data(), buf.Data() + buf.Length());
    auto worker = new CompressWorker(env, std::move(data), lossy, quality,
                                     std::move(outputPath));
    worker->Queue();
    return worker->Promise();
  }

  if (info[0].IsString()) {
    auto path = info[0].As<Napi::String>().Utf8Value();
    auto worker = new CompressWorker(env, std::move(path), lossy, quality,
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
  exports.Set("compress", Napi::Function::New(env, Compress));
  return exports;
}

NODE_API_MODULE(qpdf_compress, Init)
