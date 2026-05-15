/**
 * @ Author: Thomas N. Davies
 * @ Company: Centre for Advanced Instrumentation, Durham University
 * @ Contact: thomas.n.davies@durham.ac.uk
 * @ Create Time: 2026-05-05 22:05:45
 * @ Description: Implements FITS sample exporter.
 */

#include <sinks.hpp>

 /* Utility macro for invoking a fits API call; if the call
  * returns a unsuccessfull error code, an exception is thrown
  * with a detailed error message.
 */
#define FITS_CALL(call, ...) \
  do { \
    int status {}; \
    char err[FLEN_ERRMSG]; \
    call(__VA_ARGS__, &status); \
    if (status) { \
      fits_get_errstatus(status, err); \
      throw std::runtime_error(fmt::format( \
        "{} {}", \
        #call, err)); \
    } \
  } while (0)


 /*  -- Exporting Dao Samples to FITS --

   FITS requires NAXISx keywords where NAXIS1 is the fastest-varying
   dimension. This allows arrays exported in any storage order (row-major
   in C/Python, column-major in Fortran, etc.) to be read correctly by
   any reader, yielding logically equivalent arrays regardless of storage
   convention differences.

   Dao uses row-major ordering for multidimensional arrays (as inferred
   from its Python-Numpy API), with shape metadata as (row, col, depth)
   and array elements in row-major order.

   To export to FITS, we copy the row-major buffer directly into an
   image HDU and set NAXISx keywords to reflect the fastest-varying axis,
   enabling correct interpretation by readers with different conventions.

   Row-major storage implies:
       3D arrays: NAXIS1=depth, NAXIS2=columns, NAXIS3=rows
       2D arrays: NAXIS1=columns, NAXIS2=rows

   Therefore, reverse the shape dimension ordering before passing to FITS.
   (Note: Dao currently supports only 2D and 3D arrays.)
*/

#include <iostream>

namespace Dao::DAQ
{
    FitsWriter::FitsWriter(
        SmemParameters const& params,
        std::filesystem::path const& sessionOutputDir,
        IMAGE_METADATA const& smInfo,
        Dao::Log::Logger& log,
        std::string const& parentID
    ) :
        ISampleWriter(params, sessionOutputDir, log, parentID),
        nAxes_(smInfo.naxis),
        axes_(std::reverse_iterator(smInfo.size + nAxes_), std::reverse_iterator(smInfo.size)),
        imgType_(daoToImgType_.at(smInfo.atype)),
        srcType_(daoToSrcType_.at(smInfo.atype)),
        nSampleElements_(smInfo.nelement),
        nFileSamples(0),
        nFiles_(0),
        file_(nullptr) {
        //
        log_.Debug(LOGFMT("Fits exporter created for DAQ resource '{}'", parentID_));
    }

    /* Ensures the active datafile is safely closed before
     * the writer is destroyed. throws if an
     * issue occurred.
    */
    FitsWriter::~FitsWriter() {
        if (file_)
            closeDatafile();

        log_.Debug(LOGFMT("Fits exporter destroyed for DAQ resource '{}'", parentID_));
    }

    /* Creates a new fits datafile on the disk with the appropriate naming
     * convention, and makes it the active file for sample storage; throws
     * if an issue occurred.
    */
    void FitsWriter::newDatafile() {
        fileName_ = params_.fileRollover ?
            fmt::format("{}_{}.fits", params_.localName, nFiles_) :
            fmt::format("{}.fits", params_.localName);
        std::filesystem::path const filePath { sessionOutputDir_ / fileName_ };

        log_.Trace(LOGFMT("Creating datafile '{}' for DAQ resource '{}'", filePath.string(), parentID_));
        FITS_CALL(fits_create_file, &file_, filePath.string().c_str());
        nFileSamples = 0;
        ++nFiles_;
    }

    /* Closes the active datafile safely; throws if an
     * issue occurred.
    */
    void FitsWriter::closeDatafile() {
        FITS_CALL(fits_close_file, file_);
        log_.Debug(LOGFMT("Fits datafile '{}' closed for DAQ resource '{}'", fileName_, parentID_));
        file_ = nullptr;
    }

    /* Writes the provided shared-memory sample into the active FITS datafile.
     * throws if an issue occurred.
    */
    void FitsWriter::write(QueueType const& sample) {
        if (file_ && params_.fileRollover && params_.fileRollover.value() == nFileSamples) {
            log_.Debug(LOGFMT(
                "Fits datafile '{}' reached capacity ({} samples) for DAQ resource '{}'",
                fileName_,
                params_.fileRollover.value(),
                parentID_
            ));
            closeDatafile();
        }

        if (!file_) {
            newDatafile();
        }

        auto& [info, buffer] = const_cast<QueueType&>(sample);  // cast const away for C API.
        long* axes = const_cast<long*>(axes_.data()); // cast const away for C API.
        FITS_CALL(fits_create_img, file_, imgType_, nAxes_, axes);
        FITS_CALL(fits_write_key, file_, TBYTE, "atype", &info.atype, NULL);
        FITS_CALL(fits_write_key, file_, TLONGLONG, "atime", &info.atime.tsfixed.secondlong, "nanoseconds");
        FITS_CALL(fits_write_key, file_, TULONGLONG, "cnt0", &info.cnt0, NULL);
        FITS_CALL(fits_write_key, file_, TULONGLONG, "cnt1", &info.cnt1, NULL);
        FITS_CALL(fits_write_key, file_, TULONGLONG, "cnt2", &info.cnt2, NULL);
        FITS_CALL(fits_write_img, file_, srcType_, 1, nSampleElements_, buffer.get());
        ++nFileSamples;
    }
};
