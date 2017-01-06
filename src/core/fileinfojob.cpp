#include "fileinfojob.h"
#include "fileinfo_p.h"

namespace Fm2 {

void FileInfoJob::run() {
    for(const auto& path: paths_) {
        if(!isCancelled()) {
            GErrorPtr err;
            GFileInfoPtr inf{
                g_file_query_info(path.gfile().get(), gfile_info_query_attribs,
                                  G_FILE_QUERY_INFO_NONE, cancellable().get(), &err), false};
            std::shared_ptr<const FileInfo> fileInfo = std::make_shared<FileInfo>(inf);
            results_.push_back(fileInfo);
            Q_EMIT gotInfo(path, fileInfo);
        }
    }
    Q_EMIT finished();
}

} // namespace Fm2