#include "Project.h"
#include "FileManager.h"
#include "IndexerJob.h"
#include <rct/Rct.h>
#include <rct/Log.h>
#include <rct/MemoryMonitor.h>
#include <rct/Path.h>
#include "RTags.h"
#include <rct/ReadLocker.h>
#include <rct/RegExp.h>
#include "Server.h"
#include "Server.h"
#include "ValidateDBJob.h"
#include "IndexerJobClang.h"
#include <rct/WriteLocker.h>
#include "ReparseJob.h"
#include <math.h>

static void *ModifiedFiles = &ModifiedFiles;
static void *Save = &Save;
static void *Sync = &Sync;

enum {
    SaveTimeout = 2000,
    ModifiedFilesTimeout = 50,
    SyncTimeout = 2000
};

Project::Project(const Path &path)
    : mPath(path), mJobCounter(0)
{
    mWatcher.modified().connect(this, &Project::onFileModified);
    mWatcher.removed().connect(this, &Project::onFileModified);
    if (Server::instance()->options().options & Server::NoFileManagerWatch) {
        mWatcher.removed().connect(this, &Project::reloadFileManager);
        mWatcher.added().connect(this, &Project::reloadFileManager);
    }
}

void Project::init()
{
    assert(!isValid());
    fileManager.reset(new FileManager);
    fileManager->init(static_pointer_cast<Project>(shared_from_this()));
}

bool Project::restore()
{
    StopWatch timer;
    Path path = mPath;
    RTags::encodePath(path);
    const Path p = Server::instance()->options().dataDir + path;
    bool restoreError = false;
    FILE *f = fopen(p.constData(), "r");
    if (!f)
        return false;

    Deserializer in(f);
    int version;
    in >> version;
    if (version != Server::DatabaseVersion) {
        error("Wrong database version. Expected %d, got %d for %s. Removing.", Server::DatabaseVersion, version, p.constData());
        restoreError = true;
        goto end;
    }
    {
        int fs;
        in >> fs;
        if (fs != Rct::fileSize(f)) {
            error("%s seems to be corrupted, refusing to restore %s",
                  p.constData(), mPath.constData());
            restoreError = true;
            goto end;
        }
    }
    {

        in >> mSymbols >> mSymbolNames >> mUsr >> mDependencies >> mSources >> mVisitedFiles;

        DependencyMap reversedDependencies;
        // these dependencies are in the form of:
        // Path.cpp: Path.h, String.h ...
        // mDependencies are like this:
        // Path.h: Path.cpp, Server.cpp ...

        for (DependencyMap::const_iterator it = mDependencies.begin(); it != mDependencies.end(); ++it) {
            const Path dir = Location::path(it->first).parentDir();
            if (dir.isEmpty()) {
                error() << "File busted" << it->first << Location::path(it->first);
                continue;
            } else if (!(Server::instance()->options().options & Server::WatchSystemPaths) && dir.isSystem()) {
                continue;
            }

            if (mWatchedPaths.insert(dir))
                mWatcher.watch(dir);
            for (Set<uint32_t>::const_iterator s = it->second.begin(); s != it->second.end(); ++s) {
                reversedDependencies[*s].insert(it->first);
            }
        }

        SourceInformationMap::iterator it = mSources.begin();
        while (it != mSources.end()) {
            if (!it->second.sourceFile.isFile()) {
                error() << it->second.sourceFile << "seems to have disappeared";
                mSources.erase(it++);
                mModifiedFiles.insert(it->first);
            } else {
                const time_t parsed = it->second.parsed;
                // error() << "parsed" << String::formatTime(parsed, String::DateTime) << parsed << it->second.sourceFile;
                if (mDependencies.value(it->first).contains(it->first)) {
                    assert(mDependencies.value(it->first).contains(it->first));
                    assert(mDependencies.contains(it->first));
                    const Set<uint32_t> &deps = reversedDependencies[it->first];
                    for (Set<uint32_t>::const_iterator d = deps.begin(); d != deps.end(); ++d) {
                        if (!mModifiedFiles.contains(*d) && Location::path(*d).lastModified() > parsed) {
                            // error() << Location::path(*d).lastModified() << "is more than" << parsed;
                            mModifiedFiles.insert(*d);
                        }
                    }
                }
                ++it;
            }
        }
        if (!mModifiedFiles.isEmpty())
            startDirtyJobs();
    }
end:
    // fileManager->jsFilesChanged().connect(this, &Project::onJSFilesAdded);
    // onJSFilesAdded();
    fclose(f);
    if (restoreError) {
        Path::rm(p);
        return false;
    } else {
        error() << "Restored project" << mPath << "in" << timer.elapsed() << "ms";
    }

    return true;
}

bool Project::isValid() const
{
    return fileManager.get();
}

void Project::unload()
{
    MutexLocker lock(&mMutex);
    for (Map<uint32_t, shared_ptr<IndexerJob> >::const_iterator it = mJobs.begin(); it != mJobs.end(); ++it) {
        it->second->abort();
    }
    mJobs.clear();
    fileManager.reset();
}

bool Project::match(const Match &p, bool *indexed) const
{
    Path paths[] = { p.pattern(), p.pattern() };
    paths[1].resolve();
    const int count = paths[1].compare(paths[0]) ? 2 : 1;
    bool ret = false;
    for (int i=0; i<count; ++i) {
        const Path &path = paths[i];
        const uint32_t id = Location::fileId(path);
        if (isIndexed(id)) {
            if (indexed)
                *indexed = true;
            return true;
        } else if (mFiles.contains(path) || p.match(mPath)) {
            if (!indexed)
                return true;
            ret = true;
        }
    }
    if (indexed)
        *indexed = false;
    return ret;
}

void Project::onJobFinished(const shared_ptr<IndexerJob> &job)
{
    PendingJob pending;
    const Path currentFile = Server::instance()->currentFile();
    bool startPending = false;
    {
        MutexLocker lock(&mMutex);

        const uint32_t fileId = job->fileId();
        if (job->isAborted()) {
            mVisitedFiles -= job->visitedFiles();
            --mJobCounter;
            pending = mPendingJobs.take(fileId, &startPending);
            if (mJobs.value(fileId) == job)
                mJobs.remove(fileId);
        } else {
            assert(mJobs.value(fileId) == job);
            mJobs.remove(fileId);

            shared_ptr<IndexData> data = job->data();
            mPendingData[fileId] = data;
            if (data->type == IndexData::ClangType) {
                shared_ptr<IndexDataClang> clangData = static_pointer_cast<IndexDataClang>(data);
                if (Server::instance()->options().completionCacheSize > 0)  {
                    const SourceInformation sourceInfo = job->sourceInformation();
                    assert(sourceInfo.builds.size() == clangData->units.size());
                    for (int i=0; i<sourceInfo.builds.size(); ++i) {
                        LinkedList<CachedUnit*>::iterator it = findCachedUnit(sourceInfo.sourceFile, sourceInfo.builds.at(i).args);
                        if (it != mCachedUnits.end())
                            mCachedUnits.erase(it);
                        if (!i && currentFile == sourceInfo.sourceFile) {
                            shared_ptr<ReparseJob> rj(new ReparseJob(clangData->units.at(i).second,
                                                                     clangData->units.at(i).first,
                                                                     sourceInfo.sourceFile,
                                                                     sourceInfo.builds.at(i).args,
                                                                     static_pointer_cast<IndexerJobClang>(job)->contents(),
                                                                     static_pointer_cast<Project>(shared_from_this())));
                            Server::instance()->startIndexerJob(rj);

                        } else {
                            addCachedUnit(sourceInfo.sourceFile, sourceInfo.builds.at(i).args,
                                          clangData->units.at(i).first, clangData->units.at(i).second, 1);
                        }
                        clangData->units[i] = std::make_pair<CXIndex, CXTranslationUnit>(0, 0);
                    }
                } else {
                    clangData->clear();
                }
            }

            const int idx = mJobCounter - mJobs.size();

            mSources[fileId].parsed = job->parseTime();
            if (testLog(RTags::CompilationErrorXml))
                log(RTags::CompilationErrorXml, "<?xml version=\"1.0\" encoding=\"utf-8\"?><progress index=\"%d\" total=\"%d\"></progress>",
                    idx, mJobCounter);

            error("[%3d%%] %d/%d %s %s.",
                  static_cast<int>(round((double(idx) / double(mJobCounter)) * 100.0)), idx, mJobCounter,
                  String::formatTime(time(0), String::Time).constData(),
                  data->message.constData());

            if (mJobs.isEmpty()) {
                mSyncTimer.start(shared_from_this(), job->type() == IndexerJob::Dirty ? 0 : SyncTimeout,
                                 SingleShot, Sync);
            }
        }
    }
    if (startPending)
        index(pending.source, pending.type);
}

bool Project::save()
{
    MutexLocker lock(&mMutex);
    if (!Server::instance()->saveFileIds())
        return false;

    StopWatch timer;
    Path srcPath = mPath;
    RTags::encodePath(srcPath);
    const Server::Options &options = Server::instance()->options();
    const Path p = options.dataDir + srcPath;
    FILE *f = fopen(p.constData(), "w");
    if (!f) {
        error("Can't open file %s", p.constData());
        return false;
    }
    Serializer out(f);
    out << static_cast<int>(Server::DatabaseVersion);
    const int pos = ftell(f);
    out << static_cast<int>(0) << mSymbols << mSymbolNames << mUsr
        << mDependencies << mSources << mVisitedFiles;

    const int size = ftell(f);
    fseek(f, pos, SEEK_SET);
    out << size;

    error() << "saved project" << path() << "in" << String::format<12>("%dms", timer.elapsed()).constData();
    fclose(f);
    return true;
}

void Project::index(const SourceInformation &c, IndexerJob::Type type)
{
    MutexLocker locker(&mMutex);
    static const char *fileFilter = getenv("RTAGS_FILE_FILTER");
    if (fileFilter && !strstr(c.sourceFile.constData(), fileFilter))
        return;
    const uint32_t fileId = Location::insertFile(c.sourceFile);
    shared_ptr<IndexerJob> &job = mJobs[fileId];
    if (job) {
        if (job->abortIfStarted()) {
            const PendingJob pending = { c, type };
            mPendingJobs[fileId] = pending;
        }
        return;
    }
    shared_ptr<Project> project = static_pointer_cast<Project>(shared_from_this());

    mSources[fileId] = c;
    mPendingData.remove(fileId);

    if (!mJobCounter++)
        mTimer.start();

    job = Server::instance()->factory().createJob(project, type, c);
    if (!job) {
        error() << "Failed to create job for" << c;
        mJobs.erase(fileId);
        return;
    }
    mSyncTimer.stop();
    mSaveTimer.stop();

    Server::instance()->startIndexerJob(job);
}

static inline Path resolveCompiler(const Path &compiler)
{
    Path resolved;
    const char *linkFn;
    const char *fn;
    int fnLen;
    if (compiler.isSymLink()) {
        resolved = compiler.resolved();
        linkFn = resolved.fileName();
        fn = compiler.fileName(&fnLen);
    } else {
        linkFn = fn = compiler.fileName(&fnLen);
    }
    if (!strcmp(linkFn, "gcc-rtags-wrapper.sh") || !strcmp(linkFn, "icecc")) {
        const char *path = getenv("PATH");
        const char *last = path;
        bool done = false;
        bool found = false;
        char buf[PATH_MAX];
        while (!done) {
            switch (*path) {
            case '\0':
                done = true;
            case ':': {
                int len = (path - last);
                if (len > 0 && len + 2 + fnLen < static_cast<int>(sizeof(buf))) {
                    memcpy(buf, last, len);
                    buf[len] = '\0';
                    if (buf[len - 1] != '/')
                        buf[len++] = '/';
                    strcpy(buf + len, fn);
                    if (!access(buf, F_OK|X_OK)) {
                        if (buf == compiler) {
                            found = true;
                        } else if (found) {
                            char res[PATH_MAX];
                            buf[len + fnLen] = '\0';
                            if (realpath(buf, res)) {
                                len = strlen(res);
                                if (strcmp(res + len - 21, "/gcc-rtags-wrapper.sh") && strcmp(res + len - 6, "/icecc")) {
                                    return Path(res, len);
                                }
                                // ignore if it there's another wrapper thing in the path
                            } else {
                                return Path(buf, len + fnLen);
                            }
                        }
                    }
                }
                last = path + 1;
                break; }
            default:
                break;
            }
            ++path;
        }
    }
    if (resolved.isEmpty())
        return compiler.resolved();
    return resolved;
}

bool Project::index(const Path &sourceFile, const Path &cc, const List<String> &args)
{
    const Path compiler = resolveCompiler(cc.canonicalized());
    SourceInformation sourceInformation = sourceInfo(Location::insertFile(sourceFile));
    const bool js = args.isEmpty() && sourceFile.endsWith(".js");
    bool added = false;
    if (sourceInformation.isNull()) {
        sourceInformation.sourceFile = sourceFile;
    } else if (js) {
        debug() << sourceFile << " is not dirty. ignoring";
        return false;
    } else {
        List<SourceInformation::Build> &builds = sourceInformation.builds;
        const bool allowMultiple = Server::instance()->options().options & Server::AllowMultipleBuilds;
        for (int j=0; j<builds.size(); ++j) {
            if (builds.at(j).compiler == compiler) {
                if (builds.at(j).args == args) {
                    debug() << sourceFile << " is not dirty. ignoring";
                    return false;
                }
            }
            if (!allowMultiple) {
                builds[j].compiler = compiler;
                builds[j].args = args;
                added = true;
                break;
            }
        }
    }
    if (!added && !js)
        sourceInformation.builds.append(SourceInformation::Build(compiler, args));
    index(sourceInformation, IndexerJob::Makefile);
    return true;
}

void Project::onFileModified(const Path &file)
{
    const uint32_t fileId = Location::fileId(file);
    debug() << file << "was modified" << fileId << mModifiedFiles.contains(fileId);
    if (!fileId || !mModifiedFiles.insert(fileId)) {
        return;
    }
    if (mModifiedFiles.size() == 1 && file.isSource()) {
        startDirtyJobs();
    } else {
        mModifiedFilesTimer.start(shared_from_this(), ModifiedFilesTimeout,
                                  SingleShot, ModifiedFiles);
    }
}

SourceInformationMap Project::sourceInfos() const
{
    MutexLocker lock(&mMutex);
    return mSources;
}

SourceInformation Project::sourceInfo(uint32_t fileId) const
{
    if (fileId) {
        MutexLocker lock(&mMutex);
        return mSources.value(fileId);
    }
    return SourceInformation();
}

void Project::addDependencies(const DependencyMap &deps, Set<uint32_t> &newFiles)
{
    StopWatch timer;

    const DependencyMap::const_iterator end = deps.end();
    for (DependencyMap::const_iterator it = deps.begin(); it != end; ++it) {
        Set<uint32_t> &values = mDependencies[it->first];
        if (values.isEmpty()) {
            values = it->second;
        } else {
            values.unite(it->second);
        }
        if (newFiles.isEmpty()) {
            newFiles = it->second;
        } else {
            newFiles.unite(it->second);
        }
        newFiles.insert(it->first);
    }
}

Set<uint32_t> Project::dependencies(uint32_t fileId, DependencyMode mode) const
{
    MutexLocker lock(&mMutex);
    if (mode == DependsOnArg)
        return mDependencies.value(fileId);

    Set<uint32_t> ret;
    const DependencyMap::const_iterator end = mDependencies.end();
    for (DependencyMap::const_iterator it = mDependencies.begin(); it != end; ++it) {
        if (it->second.contains(fileId))
            ret.insert(it->first);
    }
    return ret;
}

int Project::reindex(const Match &match)
{
    Set<uint32_t> dirty;
    {
        MutexLocker lock(&mMutex);

        const DependencyMap::const_iterator end = mDependencies.end();
        for (DependencyMap::const_iterator it = mDependencies.begin(); it != end; ++it) {
            if (match.isEmpty() || match.match(Location::path(it->first))) {
                dirty.insert(it->first);
            }
        }
        if (dirty.isEmpty())
            return 0;
        mModifiedFiles += dirty;
    }
    startDirtyJobs();
    return dirty.size();
}

int Project::remove(const Match &match)
{
    int count = 0;
    {
        MutexLocker lock(&mMutex);
        SourceInformationMap::iterator it = mSources.begin();
        while (it != mSources.end()) {
            if (match.match(it->second.sourceFile)) {
                const uint32_t fileId = Location::insertFile(it->second.sourceFile);
                mSources.erase(it++);
                shared_ptr<IndexerJob> job = mJobs.value(fileId);
                if (job)
                    job->abort();
                mPendingData.remove(fileId);
                mPendingJobs.remove(fileId);
                ++count;
            } else {
                ++it;
            }
        }
    }
    return count;
}


void Project::onValidateDBJobErrors(const Set<Location> &errors)
{
    MutexLocker lock(&mMutex);
    mPreviousErrors = errors;
}

void Project::startDirtyJobs()
{
    Set<uint32_t> dirtyFiles;
    Map<Path, List<String> > toIndex;
    {
        MutexLocker lock(&mMutex);
        std::swap(dirtyFiles, mModifiedFiles);
        for (Set<uint32_t>::const_iterator it = dirtyFiles.begin(); it != dirtyFiles.end(); ++it) {
            const Set<uint32_t> deps = mDependencies.value(*it);
            dirtyFiles += deps;
            mVisitedFiles.remove(*it);
            mVisitedFiles -= deps;
        }
        mPendingDirtyFiles.unite(dirtyFiles);
    }
    bool indexed = false;
    for (Set<uint32_t>::const_iterator it = dirtyFiles.begin(); it != dirtyFiles.end(); ++it) {
        const SourceInformationMap::const_iterator found = mSources.find(*it);
        if (found != mSources.end()) {
            index(found->second, IndexerJob::Dirty);
            indexed = true;
        }
    }
    if (!indexed && !mPendingDirtyFiles.isEmpty()) {
        RTags::dirtySymbols(mSymbols, mPendingDirtyFiles);
        RTags::dirtySymbolNames(mSymbolNames, mPendingDirtyFiles);
        RTags::dirtyUsr(mUsr, mPendingDirtyFiles);
        mPendingDirtyFiles.clear();
    }
}

static inline void writeSymbolNames(const SymbolNameMap &symbolNames, SymbolNameMap &current)
{
    SymbolNameMap::const_iterator it = symbolNames.begin();
    const SymbolNameMap::const_iterator end = symbolNames.end();
    while (it != end) {
        Set<Location> &value = current[it->first];
        value.unite(it->second);
        ++it;
    }
}

static inline void joinCursors(SymbolMap &symbols, const Set<Location> &locations)
{
    for (Set<Location>::const_iterator it = locations.begin(); it != locations.end(); ++it) {
        SymbolMap::iterator c = symbols.find(*it);
        if (c != symbols.end()) {
            CursorInfo &cursorInfo = c->second;
            for (Set<Location>::const_iterator innerIt = locations.begin(); innerIt != locations.end(); ++innerIt) {
                if (innerIt != it)
                    cursorInfo.targets.insert(*innerIt);
            }
            // ### this is filthy, we could likely think of something better
        }
    }
}

static inline void writeUsr(const UsrMap &usr, UsrMap &current, SymbolMap &symbols)
{
    UsrMap::const_iterator it = usr.begin();
    const UsrMap::const_iterator end = usr.end();
    while (it != end) {
        Set<Location> &value = current[it->first];
        int count = 0;
        value.unite(it->second, &count);
        if (count && value.size() > 1)
            joinCursors(symbols, value);
        ++it;
    }
}

static inline void writeErrorSymbols(const SymbolMap &symbols, ErrorSymbolMap &errorSymbols, const Map<uint32_t, int> &errors)
{
    for (Map<uint32_t, int>::const_iterator it = errors.begin(); it != errors.end(); ++it) {
        if (it->second) {
            SymbolMap &symbolsForFile = errorSymbols[it->first];
            if (symbolsForFile.isEmpty()) {
                const Location loc(it->first, 0);
                SymbolMap::const_iterator sit = symbols.lower_bound(loc);
                while (sit != symbols.end() && sit->first.fileId() == it->first) {
                    symbolsForFile[sit->first] = sit->second;
                    ++sit;
                }
            }
        } else {
            errorSymbols.remove(it->first);
        }
    }
}

static inline void writeSymbols(SymbolMap &symbols, SymbolMap &current)
{
    if (!symbols.isEmpty()) {
        if (current.isEmpty()) {
            current = symbols;
        } else {
            SymbolMap::iterator it = symbols.begin();
            const SymbolMap::iterator end = symbols.end();
            while (it != end) {
                SymbolMap::iterator cur = current.find(it->first);
                if (cur == current.end()) {
                    current[it->first] = it->second;
                } else {
                    cur->second.unite(it->second);
                }
                ++it;
            }
        }
    }
}

static inline void writeReferences(const ReferenceMap &references, SymbolMap &symbols)
{
    const ReferenceMap::const_iterator end = references.end();
    for (ReferenceMap::const_iterator it = references.begin(); it != end; ++it) {
        const Set<Location> &refs = it->second;
        for (Set<Location>::const_iterator rit = refs.begin(); rit != refs.end(); ++rit) {
            CursorInfo &ci = symbols[*rit];
            ci.references.insert(it->first);
        }
    }
}

int Project::syncDB()
{
    if (mPendingDirtyFiles.isEmpty() && mPendingData.isEmpty())
        return -1;
    StopWatch watch;
    // for (Map<uint32_t, shared_ptr<IndexData> >::iterator it = mPendingData.begin(); it != mPendingData.end(); ++it) {
    //     writeErrorSymbols(mSymbols, mErrorSymbols, it->second->errors);
    // }

    if (!mPendingDirtyFiles.isEmpty()) {
        RTags::dirtySymbols(mSymbols, mPendingDirtyFiles);
        RTags::dirtySymbolNames(mSymbolNames, mPendingDirtyFiles);
        RTags::dirtyUsr(mUsr, mPendingDirtyFiles);
        mPendingDirtyFiles.clear();
    }

    Set<uint32_t> newFiles;
    for (Map<uint32_t, shared_ptr<IndexData> >::iterator it = mPendingData.begin(); it != mPendingData.end(); ++it) {
        const shared_ptr<IndexData> &data = it->second;
        addDependencies(data->dependencies, newFiles);
        addFixIts(data->dependencies, data->fixIts);
        writeSymbols(data->symbols, mSymbols);
        writeUsr(data->usrMap, mUsr, mSymbols);
        writeReferences(data->references, mSymbols);
        writeSymbolNames(data->symbolNames, mSymbolNames);
    }
    for (Set<uint32_t>::const_iterator it = newFiles.begin(); it != newFiles.end(); ++it) {
        const Path path = Location::path(*it);
        const Path dir = path.parentDir();
        if (dir.isEmpty()) {
            error() << "Got empty parent dir for" << path << *it;
        } else if (mWatchedPaths.insert(dir)) {
            mWatcher.watch(dir);
        }
    }
    mPendingData.clear();
    if (Server::instance()->options().options & Server::Validate) {
        shared_ptr<ValidateDBJob> validate(new ValidateDBJob(static_pointer_cast<Project>(shared_from_this()), mPreviousErrors));
        Server::instance()->startQueryJob(validate);
    }
    return watch.elapsed();
}

bool Project::isIndexed(uint32_t fileId) const
{
    MutexLocker lock(&mMutex);
    return mVisitedFiles.contains(fileId) || mSources.contains(fileId);
}

SourceInformationMap Project::sources() const
{
    MutexLocker lock(&mMutex);
    return mSources;
}
DependencyMap Project::dependencies() const
{
    MutexLocker lock(&mMutex);
    return mDependencies;
}

void Project::addCachedUnit(const Path &path, const List<String> &args, CXIndex index,
                            CXTranslationUnit unit, int parseCount) // lock always held
{
    assert(index);
    assert(unit);
    const int maxCacheSize = Server::instance()->options().completionCacheSize;
    if (!maxCacheSize) {
        clang_disposeTranslationUnit(unit);
        clang_disposeIndex(index);
        return;
    }
    CachedUnit *cachedUnit = new CachedUnit;
    cachedUnit->path = path;
    cachedUnit->index = index;
    cachedUnit->unit = unit;
    cachedUnit->arguments = args;
    cachedUnit->parseCount = parseCount;
    mCachedUnits.push_back(cachedUnit);
    while (mCachedUnits.size() > maxCacheSize) {
        CachedUnit *unit = *mCachedUnits.begin();
        delete unit;
        mCachedUnits.erase(mCachedUnits.begin());
    }
}

LinkedList<CachedUnit*>::iterator Project::findCachedUnit(const Path &path, const List<String> &args)
{
    for (LinkedList<CachedUnit*>::iterator it = mCachedUnits.begin(); it != mCachedUnits.end(); ++it) {
        if ((*it)->path == path && (args.isEmpty() || args == (*it)->arguments))
            return it;
    }
    return mCachedUnits.end();
}

bool Project::initJobFromCache(const Path &path, const List<String> &args,
                               CXIndex &index, CXTranslationUnit &unit, List<String> *argsOut,
                               int *parseCount)
{
    LinkedList<CachedUnit*>::iterator it = findCachedUnit(path, args);
    if (it != mCachedUnits.end()) {
        CachedUnit *cachedUnit = *it;
        index = cachedUnit->index;
        unit = cachedUnit->unit;
        cachedUnit->unit = 0;
        cachedUnit->index = 0;
        if (argsOut)
            *argsOut = cachedUnit->arguments;
        mCachedUnits.erase(it);
        if (parseCount)
            *parseCount = cachedUnit->parseCount;
        delete cachedUnit;
        return true;
    }
    index = 0;
    unit = 0;
    if (parseCount)
        *parseCount = -1;
    return false;
}

bool Project::fetchFromCache(const Path &path, List<String> &args, CXIndex &index, CXTranslationUnit &unit, int *parseCount)
{
    MutexLocker lock(&mMutex);
    return initJobFromCache(path, List<String>(), index, unit, &args, parseCount);
}

void Project::addToCache(const Path &path, const List<String> &args, CXIndex index, CXTranslationUnit unit, int parseCount)
{
    MutexLocker lock(&mMutex);
    addCachedUnit(path, args, index, unit, parseCount);
}

void Project::addFixIts(const DependencyMap &visited, const FixItMap &fixIts) // lock always held
{
    for (DependencyMap::const_iterator it = visited.begin(); it != visited.end(); ++it) {
        const FixItMap::const_iterator fit = fixIts.find(it->first);
        if (fit == fixIts.end()) {
            mFixIts.erase(it->first);
        } else {
            mFixIts[it->first] = fit->second;
        }
    }
}

String Project::fixIts(uint32_t fileId) const
{
    MutexLocker lock(&mMutex);
    const FixItMap::const_iterator it = mFixIts.find(fileId);
    String out;
    if (it != mFixIts.end()) {
        const Set<FixIt> &fixIts = it->second;
        if (!fixIts.isEmpty()) {
            Set<FixIt>::const_iterator f = fixIts.end();
            do {
                --f;
                if (!out.isEmpty())
                    out.append('\n');
                out.append(String::format<32>("%d-%d %s", f->start, f->end, f->text.constData()));

            } while (f != fixIts.begin());
        }
    }
    return out;
}

void Project::timerEvent(TimerEvent *e)
{
    if (e->userData() == Save) {
        save();
    } else if (e->userData() == Sync) {
        const int syncTime = syncDB();
        error() << "Jobs took" << (static_cast<double>(mTimer.elapsed()) / 1000.0) << "secs, syncing took"
                << (static_cast<double>(syncTime) / 1000.0) << " secs, using"
                << MemoryMonitor::usage() / (1024.0 * 1024.0) << "mb of memory";
        mSaveTimer.start(shared_from_this(), SaveTimeout, SingleShot, Save);
        mJobCounter = 0;
    } else if (e->userData() == ModifiedFiles) {
        startDirtyJobs();
    } else {
        assert(0 && "Unexpected timer event in Project");
        e->stop();
    }
}
void Project::onJSFilesAdded()
{
    Set<Path> jsFiles = fileManager->jsFiles();
    for (Set<Path>::const_iterator it = jsFiles.begin(); it != jsFiles.end(); ++it) {
        index(*it);
    }
}

void Project::reloadFileManager(const Path &)
{
    fileManager->reload();
}
List<std::pair<Path, List<String> > > Project::cachedUnits() const
{
    MutexLocker lock(&mMutex);
    List<std::pair<Path, List<String> > > ret;

    for (LinkedList<CachedUnit*>::const_iterator it = mCachedUnits.begin(); it != mCachedUnits.end(); ++it)
        ret.append(std::make_pair((*it)->path, (*it)->arguments));
    return ret;
}
