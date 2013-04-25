#include "ClangProject.h"
#include "RTagsPlugin.h"
#include "SourceInformation.h"
#include "QueryMessage.h"
#include "RTags.h"
#include "Server.h"
#include <rct/Connection.h>
#include <rct/LinkedList.h>
#include <rct/MutexLocker.h>
#include <rct/RegExp.h>
#include <rct/WaitCondition.h>

class ClangParseJob;

struct ClangIndexInfo
{
    ClangProject* project;
    uint32_t fileId;

    Map<Location, uint32_t> incs;
    DependSet depends, reverseDepends;
    Map<String, Set<uint32_t> > names;  // name->usr
    Map<Location, CursorInfo> usrs; // location->usr
    UsrSet decls, defs, refs; // usr->locations
    VirtualSet virtuals; // usr->usrs
    Map<Path, Set<FixIt> > fixIts;
    bool hasDiags;

    void clear()
    {
        incs.clear();
        depends.clear();
        reverseDepends.clear();
        names.clear();
        usrs.clear();
        decls.clear();
        defs.clear();
        refs.clear();
        virtuals.clear();
        fixIts.clear();
    }

    Mutex mutex;
    bool stopped;

    Map<uint32_t, bool> localSeen;

    static Mutex seenMutex;
    static Set<uint32_t> globalSeen;
};

Mutex ClangIndexInfo::seenMutex;
Set<uint32_t> ClangIndexInfo::globalSeen;

class UnitCache
{
public:
    enum { MaxSize = 5 };

    class Unit
    {
    public:
        Unit(CXTranslationUnit u) : unit(u) { }
        ~Unit() { clang_disposeTranslationUnit(unit); }

        bool operator<(const Unit& other) const { return unit < other.unit; }

        CXTranslationUnit unit;

    private:
        Unit(const Unit& other);
        Unit& operator=(const Unit& other);
    };

    static void add(const Path& path, CXTranslationUnit unit)
    {
        shared_ptr<Unit> u(new Unit(unit));
        put(path, u);
    }

    static shared_ptr<Unit> get(const Path& path)
    {
        MutexLocker locker(&mutex);
        LinkedList<std::pair<Path, shared_ptr<Unit> > >::iterator it = units.begin();
        const LinkedList<std::pair<Path, shared_ptr<Unit> > >::const_iterator end = units.end();
        while (it != end) {
            if (it->first == path) {
                shared_ptr<Unit> copy = it->second;
                units.erase(it);
                return copy;
            }
            ++it;
        }
        return shared_ptr<Unit>();
    }

    static void put(const Path& path, const shared_ptr<Unit>& unit)
    {
        MutexLocker locker(&mutex);
        assert(path.isAbsolute());
        units.push_back(std::make_pair(path, unit));
        if (units.size() > MaxSize)
            units.pop_front();
    }

private:
    static Mutex mutex;
    static LinkedList<std::pair<Path, shared_ptr<Unit> > > units;
};

Mutex UnitCache::mutex;
LinkedList<std::pair<Path, shared_ptr<UnitCache::Unit> > > UnitCache::units;

class ClangUnit
{
public:
    ClangUnit(ClangProject* project);

    void reindex(const SourceInformation& info);

    CXIndex index() { return project->cidx; }
    CXIndexAction action() { return project->caction; }

    enum MergeMode { Add = 0x1, Dirty = 0x2, DontDirtyDeps = 0x4 };
    void merge(const ClangIndexInfo& info, int mode);
    void dirty(uint32_t fileId, int mode);

    ClangProject* project;
    mutable Mutex mutex;
    SourceInformation sourceInformation;
    time_t indexed;
    shared_ptr<ClangParseJob> job;
};

class ClangParseJob : public ThreadPool::Job
{
public:
    ClangParseJob(ClangUnit* unit, bool reparse);
    ~ClangParseJob();

    void wait();
    void stop();

    // needs to be called with mUnit->mutex locked
    bool done() { return mDone; }

protected:
    virtual void run();

private:
    static int abortQuery(CXClientData client_data, void* /*reserved*/);
    static void diagnostic(CXClientData client_data, CXDiagnosticSet diags, void* /*reserved*/);
    static CXIdxClientFile enteredMainFile(CXClientData client_data, CXFile mainFile, void* /*reserved*/);
    static CXIdxClientFile includedFile(CXClientData client_data, const CXIdxIncludedFileInfo* incl);
    static void indexDeclaration(CXClientData client_data, const CXIdxDeclInfo* decl);
    static void indexEntityReference(CXClientData client_data, const CXIdxEntityRefInfo* ref);
    static void indexArguments(ClangIndexInfo* info, const CXCursor& cursor);
    static void indexMembers(ClangIndexInfo* info, const CXCursor& cursor);

    static void sendEmptyDiags(ClangIndexInfo* info);

private:
    ClangUnit* mUnit;
    bool mReparse;
    bool mDone;
    WaitCondition mWait;
    ClangIndexInfo mInfo;
};

class ClangCompletionJob : public ThreadPool::Job
{
public:
    ClangCompletionJob(ClangUnit *unit, const Location &location, const String &unsaved);
    virtual void run();
private:
    ClangUnit *mUnit;
    const Location mLocation;
    const String mUnsaved;
};

ClangUnit::ClangUnit(ClangProject* p)
    : project(p), indexed(0)
{
}

static inline void dirtyUsr(const Location& start, uint32_t usr, UsrSet& usrs)
{
    UsrSet::iterator entry = usrs.find(usr);
    if (entry == usrs.end())
        return;
    const uint32_t startFileId = start.fileId();
    Set<Location>::iterator locs = entry->second.lower_bound(start);
    while (locs != entry->second.end() && locs->fileId() == startFileId) {
        entry->second.erase(locs++);
    }
}

// should only be called with project->mutex locked
void ClangUnit::dirty(uint32_t fileId, int mode)
{
    assert(mode & ClangUnit::Dirty);

    const Location start(fileId, 1, 1);
    {
        Map<Location, CursorInfo>::iterator usr = project->usrs.lower_bound(start);
        while (usr != project->usrs.end() && usr->first.fileId() == fileId) {
            dirtyUsr(start, usr->second.usr, project->decls);
            dirtyUsr(start, usr->second.usr, project->defs);
            dirtyUsr(start, usr->second.usr, project->refs);
            project->usrs.erase(usr++);
        }
    }

    if (!(mode & ClangUnit::DontDirtyDeps)) {
        {
            Map<Location, uint32_t>::iterator inc = project->incs.lower_bound(start);
            const Map<Location, uint32_t>::const_iterator end = project->incs.end();
            while (inc != end && inc->first.fileId() == fileId) {
                project->incs.erase(inc++);
            }
        }
        {
            // remove headers?
            DependSet::iterator dep = project->depends.find(fileId);
            if (dep != project->depends.end()) {
                project->depends.erase(dep);
            }
        }
        {
            DependSet::iterator dep = project->reverseDepends.begin();
            while (dep != project->reverseDepends.end()) {
                Set<uint32_t>& set = dep->second;
                if (set.remove(fileId)) {
                    if (set.isEmpty()) {
                        project->reverseDepends.erase(dep++);
                    } else
                        ++dep;
                } else {
                    ++dep;
                }
            }
        }
    }
}

void ClangUnit::merge(const ClangIndexInfo& info, int mode)
{
    MutexLocker locker(&project->mutex);

    --project->pendingJobs;

    if (mode & Dirty)
        dirty(sourceInformation.sourceFileId(), mode);

    project->incs.unite(info.incs);
    project->usrs.unite(info.usrs);
    project->fixIts.unite(info.fixIts);

    {
        Map<String, Set<uint32_t> >::const_iterator name = info.names.begin();
        const Map<String, Set<uint32_t> >::const_iterator end = info.names.end();
        while (name != end) {
            project->names[name->first].unite(name->second);
            ++name;
        }
    }
    {
        const UsrSet* src[] = { &info.decls, &info.defs, &info.refs, 0 };
        UsrSet* dst[] = { &project->decls, &project->defs, &project->refs, 0 };
        for (unsigned i = 0; src[i]; ++i) {
            UsrSet::const_iterator usr = src[i]->begin();
            const UsrSet::const_iterator end = src[i]->end();
            while (usr != end) {
                (*dst[i])[usr->first].unite(usr->second);
                ++usr;
            }
        }
    }
    {
        const DependSet* src[] = { &info.depends, &info.reverseDepends, 0 };
        DependSet* dst[] = { &project->depends, &project->reverseDepends, 0 };
        for (unsigned i = 0; src[i]; ++i) {
            DependSet::const_iterator usr = src[i]->begin();
            const DependSet::const_iterator end = src[i]->end();
            while (usr != end) {
                (*dst[i])[usr->first].unite(usr->second);
                ++usr;
            }
        }
    }

    VirtualSet::const_iterator virt = info.virtuals.begin();
    const VirtualSet::const_iterator end = info.virtuals.end();
    while (virt != end) {
        project->virtuals[virt->first].unite(virt->second);
        ++virt;
    }
    if (!project->pendingJobs) {
        error() << "Parsed" << project->jobsProcessed << "files in" << project->timer.elapsed() << "ms";
        project->jobsProcessed = 0;
        project->save(); // should I release the mutex first?
    }
}

ClangParseJob::ClangParseJob(ClangUnit* unit, bool reparse)
    : mUnit(unit), mReparse(reparse), mDone(false)
{
    mInfo.stopped = false;
    mInfo.project = mUnit->project;
    mInfo.fileId = mUnit->sourceInformation.sourceFileId();
    mInfo.hasDiags = false;
}

ClangParseJob::~ClangParseJob()
{
}

static inline Location makeLocation(const CXIdxLoc& cxloc, unsigned* offset = 0)
{
    CXIdxClientFile file = 0;
    CXFile cxfile = 0;
    unsigned line, column;
    clang_indexLoc_getFileLocation(cxloc, &file, &cxfile, &line, &column, offset);

    uint32_t fileId;
    if (file) {
        fileId = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(file));
    } else {
        // fall back to CXFile
        if (!cxfile)
            return Location();
        CXString fn = clang_getFileName(cxfile);
        const char* fileName = clang_getCString(fn);
        if (fileName) {
            fileId = Location::insertFile(Path::resolved(clang_getCString(fn)));
        } else {
            return Location();
        }
        clang_disposeString(fn);
    }
    return Location(fileId, line, column);
}

static inline Location makeLocation(const CXCursor& cursor, unsigned* offset = 0)
{
    const CXSourceLocation cxloc = clang_getCursorLocation(cursor);
    if (clang_equalLocations(cxloc, clang_getNullLocation()))
        return Location();
    CXFile file;
    unsigned line, column;
    clang_getSpellingLocation(cxloc, &file, &line, &column, offset);
    if (!file)
        return Location();
    CXString fileName = clang_getFileName(file);
    const uint32_t fileId = Location::insertFile(Path::resolved(clang_getCString(fileName)));
    const Location loc(fileId, line, column);
    clang_disposeString(fileName);
    return loc;
}

static inline Project::Cursor::Kind makeKind(CXIdxEntityKind cxkind, bool def)
{
    switch (cxkind) {
    case CXIdxEntity_CXXClass:
        if (def)
            return Project::Cursor::Class;
        return Project::Cursor::ClassForwardDeclaration;
    case CXIdxEntity_CXXNamespace:
        return Project::Cursor::Namespace;
    case CXIdxEntity_CXXInstanceMethod:
    case CXIdxEntity_CXXConstructor:
    case CXIdxEntity_CXXDestructor:
    case CXIdxEntity_CXXStaticMethod:
        if (def)
            return Project::Cursor::MemberFunctionDefinition;
        return Project::Cursor::MemberFunctionDeclaration;
    case CXIdxEntity_Function:
        if (def)
            return Project::Cursor::MethodDefinition;
        return Project::Cursor::MethodDeclaration;
    case CXIdxEntity_Struct:
        if (def)
            return Project::Cursor::Struct;
        return Project::Cursor::StructForwardDeclaration;
    case CXIdxEntity_Enum:
        return Project::Cursor::Enum;
    case CXIdxEntity_EnumConstant:
        return Project::Cursor::EnumValue;
    case CXIdxEntity_Variable:
    case CXIdxEntity_CXXStaticVariable:
        return Project::Cursor::Variable;
    case CXIdxEntity_Field:
        return Project::Cursor::Field;
    case CXIdxEntity_Union:
        return Project::Cursor::Union;
    case CXIdxEntity_Unexposed:
    case CXIdxEntity_Typedef:
    case CXIdxEntity_ObjCClass:
    case CXIdxEntity_ObjCProtocol:
    case CXIdxEntity_ObjCCategory:
    case CXIdxEntity_ObjCInstanceMethod:
    case CXIdxEntity_ObjCClassMethod:
    case CXIdxEntity_ObjCProperty:
    case CXIdxEntity_ObjCIvar:
    case CXIdxEntity_CXXConversionFunction:
    case CXIdxEntity_CXXNamespaceAlias:
    case CXIdxEntity_CXXTypeAlias:
    case CXIdxEntity_CXXInterface:
        break;
    }
    return Project::Cursor::Invalid;
}

int ClangParseJob::abortQuery(CXClientData client_data, void* /*reserved*/)
{
    ClangIndexInfo* info = static_cast<ClangIndexInfo*>(client_data);
    MutexLocker locker(&info->mutex);
    return info->stopped ? 1 : 0;
}

static inline void addDeps(uint32_t fileId, const DependSet& deps, Set<uint32_t>& result)
{
    DependSet::const_iterator dep = deps.find(fileId);
    if (dep != deps.end()) {
        Set<uint32_t>::const_iterator path = dep->second.begin();
        const Set<uint32_t>::const_iterator end = dep->second.end();
        while (path != end) {
            if (!result.contains(*path)) {
                result.insert(*path);
                addDeps(*path, deps, result);
            }
            ++path;
        }
    }
}

struct XmlEntry
{
    enum Type { None, Warning, Error, Fixit };

    XmlEntry(Type t = None, const String& m = String(), int l = 0, int c = 0, int eo = -1)
        : type(t), message(m), line(l), column(c), endOffset(eo)
    {
    }

    Type type;
    String message;
    int line, column, endOffset;
};

static inline String xmlEscape(const String& xml)
{
    if (xml.isEmpty())
        return xml;

    std::ostringstream strm;
    const char* ch = xml.constData();
    bool done = false;
    for (;;) {
        switch (*ch) {
        case '\0':
            done = true;
            break;
        case '"':
            strm << "\\\"";
            break;
        case '<':
            strm << "&lt;";
            break;
        case '>':
            strm << "&gt;";
            break;
        case '&':
            strm << "&amp;";
            break;
        default:
            strm << *ch;
            break;
        }
        if (done)
            break;
        ++ch;
    }
    return strm.str();
}

static inline Path path(const CXFile &file)
{
    const CXString fn = clang_getFileName(file);
    const Path path = Path::resolved(clang_getCString(fn));
    clang_disposeString(fn);
    return path;
}

void ClangParseJob::diagnostic(CXClientData client_data, CXDiagnosticSet diags, void* /*reserved*/)
{
    ClangIndexInfo* info = static_cast<ClangIndexInfo*>(client_data);

    const unsigned diagnosticCount = clang_getNumDiagnosticsInSet(diags);
    const unsigned options = Server::options().options;

    info->hasDiags = info->hasDiags || diagnosticCount;

    Map<Path, Map<unsigned, XmlEntry> > xmlEntries;
    const bool xmlEnabled = testLog(RTags::CompilationErrorXml);

    for (unsigned i=0; i<diagnosticCount; ++i) {
        CXDiagnostic diagnostic = clang_getDiagnosticInSet(diags, i);
        int logLevel = INT_MAX;
        const CXDiagnosticSeverity severity = clang_getDiagnosticSeverity(diagnostic);
        switch (severity) {
        case CXDiagnostic_Fatal:
        case CXDiagnostic_Error:
            logLevel = Error;
            break;
        case CXDiagnostic_Warning:
            logLevel = Warning;
            break;
        case CXDiagnostic_Note:
            logLevel = Debug;
            break;
        case CXDiagnostic_Ignored:
            break;
        }

        const CXSourceLocation diagLoc = clang_getDiagnosticLocation(diagnostic);
        CXString cxstr = clang_getDiagnosticSpelling(diagnostic);
        const String msg(clang_getCString(cxstr));
        clang_disposeString(cxstr);
        if (xmlEnabled) {
            const CXDiagnosticSeverity sev = clang_getDiagnosticSeverity(diagnostic);
            XmlEntry::Type type = XmlEntry::None;
            switch (sev) {
            case CXDiagnostic_Warning:
                type = XmlEntry::Warning;
                break;
            case CXDiagnostic_Error:
            case CXDiagnostic_Fatal:
                type = XmlEntry::Error;
                break;
            default:
                break;
            }
            if (type != XmlEntry::None) {
                const unsigned rangeCount = clang_getDiagnosticNumRanges(diagnostic);
                bool rangeOk = rangeCount;
                for (unsigned rangePos = 0; rangePos < rangeCount; ++rangePos) {
                    const CXSourceRange range = clang_getDiagnosticRange(diagnostic, rangePos);
                    const CXSourceLocation start = clang_getRangeStart(range);
                    const CXSourceLocation end = clang_getRangeEnd(range);

                    unsigned line, column, startOffset, endOffset;
                    CXFile file;
                    clang_getSpellingLocation(start, &file, &line, &column, &startOffset);
                    Path p = ::path(file);
                    clang_getSpellingLocation(end, 0, 0, 0, &endOffset);
                    if (!rangePos && !startOffset && !endOffset) {
                        rangeOk = false;
                        // huh, range invalid? fall back to diag location
                        break;
                    } else {
                        xmlEntries[p][startOffset] = XmlEntry(type, msg, line, column, endOffset);
                    }
                }
                if (!rangeOk) {
                    unsigned line, column, offset;
                    CXFile file;
                    clang_getSpellingLocation(diagLoc, &file, &line, &column, &offset);
                    xmlEntries[::path(file)][offset] = XmlEntry(type, msg, line, column);
                }
            }
            if (testLog(logLevel) || testLog(RTags::CompilationError)) {
                if (testLog(logLevel))
                    logDirect(logLevel, msg.constData());
                if (testLog(RTags::CompilationError))
                    logDirect(RTags::CompilationError, msg.constData());
            }

            const unsigned fixItCount = clang_getDiagnosticNumFixIts(diagnostic);
            RegExp rx;
            if (options & Server::IgnorePrintfFixits) {
                rx = "^%[A-Za-z0-9]\\+$";
            }
            for (unsigned f=0; f<fixItCount; ++f) {
                CXSourceRange range;
                const CXString diagnosticString = clang_getDiagnosticFixIt(diagnostic, f, &range);
                unsigned startOffset, line, column, endOffset;
                CXFile file;
                clang_getSpellingLocation(clang_getRangeStart(range), &file, &line, &column, &startOffset);
                clang_getSpellingLocation(clang_getRangeEnd(range), 0, 0, 0, &endOffset);

                const Path p = ::path(file);
                const char *string = clang_getCString(diagnosticString);
                if (options & Server::IgnorePrintfFixits && rx.indexIn(string) == 0) {
                    error("Ignored fixit for %s: Replace %d-%d with [%s]", p.constData(),
                          startOffset, endOffset, string);
                    continue;
                }

                // error("Fixit for %s: Replace %d-%d with [%s]", p.constData(), startOffset, endOffset, string);

                if (xmlEnabled) {
                    XmlEntry& entry = xmlEntries[p][startOffset];
                    entry.type = XmlEntry::Fixit;
                    if (entry.message.isEmpty()) {
                        entry.message = String::format<64>("did you mean '%s'?", string);
                        entry.line = line;
                        entry.column = column;
                    }
                    entry.endOffset = endOffset;
                }
                if (testLog(logLevel) || testLog(RTags::CompilationError)) {
                    const String msg = String::format<128>("Fixit for %s: Replace %d-%d with [%s]", p.constData(),
                                                           startOffset, endOffset, string);
                    if (testLog(logLevel))
                        logDirect(logLevel, msg.constData());
                    if (testLog(RTags::CompilationError))
                        logDirect(RTags::CompilationError, msg.constData());
                }
                info->fixIts[p].insert(FixIt(startOffset, endOffset, string));
            }
        }

        clang_disposeDiagnostic(diagnostic);
    }
    if (xmlEnabled) {
        logDirect(RTags::CompilationErrorXml, "<?xml version=\"1.0\" encoding=\"utf-8\"?><checkstyle>");
        if (!xmlEntries.isEmpty()) {
            Map<Path, Map<unsigned, XmlEntry> >::const_iterator entry = xmlEntries.begin();
            const Map<Path, Map<unsigned, XmlEntry> >::const_iterator end = xmlEntries.end();

            const char* severities[] = { "none", "warning", "error", "fixit" };

            while (entry != end) {
                log(RTags::CompilationErrorXml, "<file name=\"%s\">", entry->first.constData());
                const Map<unsigned, XmlEntry>& map = entry->second;
                Map<unsigned, XmlEntry>::const_iterator it = map.begin();
                const Map<unsigned, XmlEntry>::const_iterator end = map.end();
                while (it != end) {
                    const XmlEntry& entry = it->second;
                    log(RTags::CompilationErrorXml, "<error line=\"%d\" column=\"%d\" startOffset=\"%d\" %sseverity=\"%s\" message=\"%s\"/>",
                        entry.line, entry.column, it->first,
                        (entry.endOffset == -1 ? "" : String::format<32>("endOffset=\"%d\" ", entry.endOffset).constData()),
                        severities[entry.type], xmlEscape(entry.message).constData());
                    ++it;
                }
                logDirect(RTags::CompilationErrorXml, "</file>");
                ++entry;
            }
        }

        Set<Path> files;
        {
            MutexLocker locker(&info->project->mutex);
            Set<uint32_t> deps;

            addDeps(info->fileId, info->project->depends, deps);
            Set<uint32_t>::const_iterator it = deps.begin();
            const Set<uint32_t>::const_iterator end = deps.end();
            while (it != end) {
                files.insert(Location::path(*it));
                ++it;
            }
        }

        for (Set<Path>::const_iterator it = files.begin(); it != files.end(); ++it) {
            if (!xmlEntries.contains(*it)) {
                log(RTags::CompilationErrorXml, "<file name=\"%s\"/>", it->constData());
            }
        }

        logDirect(RTags::CompilationErrorXml, "</checkstyle>");
    }
}

void ClangParseJob::sendEmptyDiags(ClangIndexInfo* info)
{
    const bool xmlEnabled = testLog(RTags::CompilationErrorXml);
    if (!xmlEnabled)
        return;

    Set<Path> files;
    {
        MutexLocker locker(&info->project->mutex);
        Set<uint32_t> deps;

        deps.insert(info->fileId);

        addDeps(info->fileId, info->project->depends, deps);
        Set<uint32_t>::const_iterator it = deps.begin();
        const Set<uint32_t>::const_iterator end = deps.end();
        while (it != end) {
            files.insert(Location::path(*it));
            ++it;
        }
    }

    logDirect(RTags::CompilationErrorXml, "<?xml version=\"1.0\" encoding=\"utf-8\"?><checkstyle>");
    for (Set<Path>::const_iterator it = files.begin(); it != files.end(); ++it) {
        log(RTags::CompilationErrorXml, "<file name=\"%s\"/>", it->constData());
    }
    logDirect(RTags::CompilationErrorXml, "</checkstyle>");
}

CXIdxClientFile ClangParseJob::enteredMainFile(CXClientData client_data, CXFile mainFile, void* /*reserved*/)
{
    CXString str = clang_getFileName(mainFile);
    const uint32_t fileId = Location::insertFile(Path::resolved(clang_getCString(str)));
    clang_disposeString(str);
    return reinterpret_cast<CXIdxClientFile>(fileId);
}

CXIdxClientFile ClangParseJob::includedFile(CXClientData client_data, const CXIdxIncludedFileInfo* incl)
{
    ClangIndexInfo* info = static_cast<ClangIndexInfo*>(client_data);
    CXString str = clang_getFileName(incl->file);
    const Path path = Path::resolved(clang_getCString(str));
    clang_disposeString(str);
    const Location loc = makeLocation(incl->hashLoc);
    if (loc.isEmpty())
        return 0;
    const uint32_t fileId = Location::insertFile(path);

    info->depends[loc.fileId()].insert(fileId);
    info->reverseDepends[fileId].insert(loc.fileId());
    info->incs[loc] = fileId;

    return reinterpret_cast<CXIdxClientFile>(fileId);
}

static inline uint32_t makeUsr(const CXCursor& cursor)
{
    CXString str = clang_getCursorUSR(cursor);
    const uint32_t usr = ClangProject::usrMap().insert(clang_getCString(str));
    clang_disposeString(str);
    return usr;
}

static inline unsigned cursorLength(const CXCursor& cursor)
{
    CXString cxname = clang_getCursorSpelling(cursor);
    const char* cstr = clang_getCString(cxname);
    const unsigned len = cstr ? strlen(cstr) : 0;
    clang_disposeString(cxname);
    return len;
}

static inline void addReference(CXClientData client_data, CXCursor cursor)
{
    ClangIndexInfo* info = static_cast<ClangIndexInfo*>(client_data);
    unsigned offset;
    const Location refLoc = makeLocation(cursor, &offset);
    if (refLoc.isEmpty())
        return;
    const uint32_t usr = makeUsr(clang_getCursorReferenced(cursor));

    CursorInfo cursorInfo;
    cursorInfo.usr = usr;
    cursorInfo.kind = Project::Cursor::Reference;
    cursorInfo.start = offset;
    cursorInfo.end = offset + cursorLength(cursor);
    info->usrs[refLoc] = cursorInfo;

    //error() << "indexing ref" << usr << refLoc;

    info->refs[usr].insert(refLoc);
}

static CXChildVisitResult argumentVisistor(CXCursor cursor, CXCursor /*parent*/, CXClientData client_data)
{
    switch (clang_getCursorKind(cursor)) {
    case CXCursor_ParmDecl:
        return CXChildVisit_Recurse;
    case CXCursor_TypeRef:
        addReference(client_data, cursor);
        return CXChildVisit_Continue;
    default:
        break;
    }
    return CXChildVisit_Break;
}

static CXChildVisitResult memberVisistor(CXCursor cursor, CXCursor /*parent*/, CXClientData client_data)
{
    //error() << "found" << clang_getCursorKind(cursor);
    switch (clang_getCursorKind(cursor)) {
    case CXCursor_FieldDecl:
    case CXCursor_CXXBaseSpecifier:
        return CXChildVisit_Recurse;
    case CXCursor_TypeRef:
        addReference(client_data, cursor);
        break;
    default:
        break;
    }
    return CXChildVisit_Continue;
}

void ClangParseJob::indexArguments(ClangIndexInfo* info, const CXCursor& cursor)
{
    clang_visitChildren(cursor, argumentVisistor, info);
}

void ClangParseJob::indexMembers(ClangIndexInfo* info, const CXCursor& cursor)
{
    clang_visitChildren(cursor, memberVisistor, info);
}

static inline void addNamePermutations(CXCursor cursor, const uint32_t usr, Map<String, Set<uint32_t> >& names)
{
    List<String> subnames;
    unsigned res = 0;
    for (;;) {
        if (clang_isDeclaration(clang_getCursorKind(cursor))) {
            CXString cxname = clang_getCursorSpelling(cursor);
            String name(clang_getCString(cxname));
            clang_disposeString(cxname);
            if (!name.isEmpty()) {
                subnames.append(name);
                res += name.size();
            } else if (subnames.isEmpty()) {
                break;
            }
        }
        cursor = clang_getCursorSemanticParent(cursor);
        if (clang_equalCursors(cursor, clang_getNullCursor()))
            break;
    }

    if (subnames.isEmpty())
        return;

    String current;
    current.reserve(res + ((subnames.size() - 1) * 2));
    List<String>::const_iterator n = subnames.begin();
    const List<String>::const_iterator end = subnames.end();
    while (n != end) {
        if (!current.isEmpty())
            current.prepend("::");
        current.prepend(*n);
        names[current].insert(usr);
        ++n;
    }
}

void ClangParseJob::indexDeclaration(CXClientData client_data, const CXIdxDeclInfo* decl)
{
    ClangIndexInfo* info = static_cast<ClangIndexInfo*>(client_data);
    unsigned offset;
    const Location declLoc = makeLocation(decl->loc, &offset);
    if (!decl->entityInfo->USR || declLoc.isEmpty())
        return;

    switch (decl->entityInfo->templateKind) {
    case CXIdxEntity_NonTemplate: {
        // Hack, typedefs for templates are not actually template entities. Allow them all for now.
        // ### better/possible to get the referenced symbol here?
        if (decl->entityInfo->kind == CXIdxEntity_Typedef)
            break;
        const uint32_t fileId = declLoc.fileId();
        const Map<uint32_t, bool>::const_iterator seen = info->localSeen.find(fileId);
        if (seen != info->localSeen.end()) {
            if (!seen->second)
                return;
        } else {
            MutexLocker locker(&ClangIndexInfo::seenMutex);
            if (!ClangIndexInfo::globalSeen.insert(fileId)) {
                info->localSeen[fileId] = false;
                return;
            }
            info->localSeen[fileId] = true;
        }
        break; }
    default:
        break;
    }

    const bool def = decl->isDefinition;
    const uint32_t usr = ClangProject::usrMap().insert(decl->entityInfo->USR);

    CursorInfo cursorInfo;
    cursorInfo.usr = usr;
    cursorInfo.kind = makeKind(decl->entityInfo->kind, def);
    cursorInfo.start = offset;
    cursorInfo.end = offset + cursorLength(decl->cursor);
    info->usrs[declLoc] = cursorInfo;

    //error() << "indexing" << (def ? "def" : "decl") << decl->entityInfo->kind << usr << declLoc;
    switch (decl->entityInfo->kind) {
    case CXIdxEntity_CXXInstanceMethod:
        if (clang_CXXMethod_isVirtual(decl->cursor)) {
            //error() << "virtual at" << makeLocation(decl->cursor);
            CXCursor* overridden;
            unsigned num;
            clang_getOverriddenCursors(decl->cursor, &overridden, &num);
            if (num) {
                uint32_t virtUsr;
                for (unsigned i = 0; i < num; ++i) {
                    virtUsr = makeUsr(overridden[i]);
                    //error() << "overridden at" << makeLocation(overridden[i]) << virtusr;
                    info->virtuals[usr].insert(virtUsr);
                    info->virtuals[virtUsr].insert(usr);
                }
                clang_disposeOverriddenCursors(overridden);
            }
        }
        // fall through
    case CXIdxEntity_CXXStaticMethod:
    case CXIdxEntity_CXXConstructor:
    case CXIdxEntity_Function:
        indexArguments(info, decl->cursor);
        break;
    case CXIdxEntity_CXXClass:
    case CXIdxEntity_Struct:
    case CXIdxEntity_Union:
        indexMembers(info, decl->cursor);
        break;
    default:
        break;
    }

    addNamePermutations(decl->cursor, usr, info->names);

    if (def)
        info->defs[usr].insert(declLoc);
    else
        info->decls[usr].insert(declLoc);
}

void ClangParseJob::indexEntityReference(CXClientData client_data, const CXIdxEntityRefInfo* ref)
{
    ClangIndexInfo* info = static_cast<ClangIndexInfo*>(client_data);
    unsigned offset;
    const Location refLoc = makeLocation(ref->loc, &offset);
    if (!ref->referencedEntity->USR || refLoc.isEmpty())
        return;

    {
        const uint32_t fileId = refLoc.fileId();
        const Map<uint32_t, bool>::const_iterator seen = info->localSeen.find(fileId);
        if (seen != info->localSeen.end()) {
            if (!seen->second)
                return;
        } else {
            MutexLocker locker(&ClangIndexInfo::seenMutex);
            if (!ClangIndexInfo::globalSeen.insert(fileId)) {
                info->localSeen[fileId] = false;
                return;
            }
            info->localSeen[fileId] = true;
        }
    }

    const uint32_t usr = ClangProject::usrMap().insert(ref->referencedEntity->USR);

    CursorInfo cursorInfo;
    cursorInfo.usr = usr;
    cursorInfo.kind = Project::Cursor::Reference;
    cursorInfo.start = offset;
    cursorInfo.end = offset + cursorLength(ref->cursor);
    info->usrs[refLoc] = cursorInfo;

    //error() << "indexing ref" << usr << refLoc;

    info->refs[usr].insert(refLoc);
}

void ClangParseJob::stop()
{
    MutexLocker locker(&mInfo.mutex);
    mInfo.stopped = true;
}

void ClangParseJob::wait()
{
    mWait.wait(&mUnit->mutex);
}

static CXChildVisitResult hasInclusionsVisitor(CXCursor cursor, CXCursor /*parent*/, CXClientData client_data)
{
    if (clang_getCursorKind(cursor) == CXCursor_InclusionDirective) {
        *static_cast<bool*>(client_data) = true;
        return CXChildVisit_Break;
    }
    return CXChildVisit_Continue;
}

static inline bool hasInclusions(CXTranslationUnit unit)
{
    CXCursor top = clang_getTranslationUnitCursor(unit);
    bool has = false;
    clang_visitChildren(top, hasInclusionsVisitor, &has);
    return has;
}

void ClangParseJob::run()
{
    const Path sourceFile = mUnit->sourceInformation.sourceFile;

    {
        MutexLocker locker(&mInfo.mutex);
        if (mInfo.stopped) {
            MutexLocker locker(&mUnit->mutex);
            mDone = true;
            mWait.wakeOne();
            return;
        }
    }

    // clang parse
    time_t parseTime = 0;
    if (mReparse) {
        // ### should handle multiple builds here
        shared_ptr<UnitCache::Unit> unitptr = UnitCache::get(sourceFile);
        if (unitptr) {
            CXTranslationUnit unit = unitptr->unit;
            if (clang_reparseTranslationUnit(unit, 0, 0, clang_defaultReparseOptions(unit)) != 0) {
                // bad
                mInfo.clear();
                mReparse = false;
            } else {
                IndexerCallbacks callbacks;
                memset(&callbacks, 0, sizeof(IndexerCallbacks));
                callbacks.abortQuery = abortQuery;
                callbacks.diagnostic = diagnostic;
                callbacks.enteredMainFile = enteredMainFile;
                callbacks.ppIncludedFile = includedFile;
                callbacks.indexDeclaration = indexDeclaration;
                callbacks.indexEntityReference = indexEntityReference;
                const unsigned opts = CXIndexOpt_IndexFunctionLocalSymbols | CXIndexOpt_IndexImplicitTemplateInstantiations;

                int dirtyFlags = ClangUnit::Dirty;

                if (clang_indexTranslationUnit(mUnit->action(), &mInfo, &callbacks, sizeof(IndexerCallbacks), opts, unit)) {
                    parseTime = time(0);
                    mInfo.clear();
                    mReparse = false;
                } else {
                    if (hasInclusions(unit) && mInfo.depends.isEmpty())
                        dirtyFlags |= ClangUnit::DontDirtyDeps;
                    parseTime = time(0);
                }

                {
                    MutexLocker locker(&mInfo.mutex);
                    if (mInfo.stopped) {
                        MutexLocker locker(&mUnit->mutex);
                        mDone = true;
                        mWait.wakeOne();
                        return;
                    }
                }

                if (mReparse) {
                    if (!mInfo.hasDiags)
                        sendEmptyDiags(&mInfo);
                    mUnit->merge(mInfo, dirtyFlags);
                }
            }
        } else {
            mReparse = false;
        }

        if (mReparse) {
            // all ok
            assert(unitptr != 0);
            assert(parseTime);
            UnitCache::put(sourceFile, unitptr);
        }
    }
    if (!mReparse) {
        const List<SourceInformation::Build>& builds = mUnit->sourceInformation.builds;
        List<SourceInformation::Build>::const_iterator build = builds.begin();
        const List<SourceInformation::Build>::const_iterator end = builds.end();
        while (build != end) {
            List<String> args;
#ifdef CLANG_INCLUDEPATH
            args.append(String("-I") + CLANG_INCLUDEPATH);
#endif

            List<String>::const_iterator define = build->defines.begin();
            const List<String>::const_iterator defineEnd = build->defines.end();
            while (define != defineEnd) {
                args.append("-D" + *define);
                ++define;
            }

            List<Path>::const_iterator include = build->includePaths.begin();
            List<Path>::const_iterator includeEnd = build->includePaths.end();
            while (include != includeEnd) {
                args.append("-I" + *include);
                ++include;
            }

            include = build->includes.begin();
            includeEnd = build->includes.end();
            while (include != includeEnd) {
                args.append("-include " + *include);
                ++include;
            }

            const char* clangArgs[args.size()];
            int clangOffset = 0;
            List<String>::const_iterator arg = args.begin();
            const List<String>::const_iterator argEnd = args.end();
            while (arg != argEnd) {
                clangArgs[clangOffset++] = arg->nullTerminated();
                ++arg;
            }

            IndexerCallbacks callbacks;
            memset(&callbacks, 0, sizeof(IndexerCallbacks));
            callbacks.abortQuery = abortQuery;
            callbacks.diagnostic = diagnostic;
            callbacks.enteredMainFile = enteredMainFile;
            callbacks.ppIncludedFile = includedFile;
            callbacks.indexDeclaration = indexDeclaration;
            callbacks.indexEntityReference = indexEntityReference;
            const unsigned opts = CXIndexOpt_IndexFunctionLocalSymbols | CXIndexOpt_IndexImplicitTemplateInstantiations;
            const unsigned tuOpts =
                CXTranslationUnit_DetailedPreprocessingRecord | CXTranslationUnit_PrecompiledPreamble | CXTranslationUnit_CacheCompletionResults;

            CXTranslationUnit unit = 0;
            if (clang_indexSourceFile(mUnit->action(), &mInfo, &callbacks, sizeof(IndexerCallbacks), opts,
				      sourceFile.nullTerminated(), clangArgs, args.size(), 0, 0, &unit, tuOpts)) {
                if (unit) {
                    clang_disposeTranslationUnit(unit);
                    unit = 0;
                }
		mInfo.clear();
            } else {
                assert(!parseTime);
                parseTime = time(0);
            }

            {
                MutexLocker locker(&mInfo.mutex);
                if (mInfo.stopped) {
                    MutexLocker locker(&mUnit->mutex);
                    mDone = true;
                    mWait.wakeOne();
                    return;
                }
            }

            if (unit) {
                assert(parseTime);
                UnitCache::add(sourceFile, unit);

                if (!mInfo.hasDiags)
                    sendEmptyDiags(&mInfo);
            }

            mUnit->merge(mInfo, build == builds.begin() ? ClangUnit::Dirty : ClangUnit::Add);

            ++build;
        }
    }

    error() << "done parsing" << mUnit->sourceInformation.sourceFile << "reparse" << mReparse;

    MutexLocker locker(&mUnit->mutex);
    mUnit->indexed = parseTime;
    mDone = true;
    mWait.wakeOne();
}

void ClangUnit::reindex(const SourceInformation& info)
{
    {
        MutexLocker locker(&ClangIndexInfo::seenMutex);
        const uint32_t fileId = Location::fileId(info.sourceFile);
        if (fileId && ClangIndexInfo::globalSeen.contains(fileId)) {
            // the file has already been indexed, we need to take out the fileid from the seen list
            ClangIndexInfo::globalSeen.remove(fileId);

            // ### do we need to take out all the deps as well?
            Set<uint32_t> deps;
            {
                MutexLocker locker(&project->mutex);
                addDeps(fileId, project->depends, deps);
            }
            ClangIndexInfo::globalSeen.subtract(deps);
        }
    }

    MutexLocker locker(&mutex);
    if (job) {
        while (!job->done()) {
            if (!project->pool.remove(job)) {
                job->stop();
                job->wait();
            } else {
                break;
            }
        }
    }

    const bool reparse = (sourceInformation == info);
    if (!reparse)
        sourceInformation = info;
    job.reset(new ClangParseJob(this, reparse));
    project->pool.start(job);
}

LockingUsrMap ClangProject::umap;

ClangProject::ClangProject(const Path &path)
    : Project(path), pool(Server::options().threadPoolSize, Server::options().threadPoolStackSize),
      pendingJobs(0), jobsProcessed(0)
{
    cidx = clang_createIndex(0, 1);
    caction = clang_IndexAction_create(cidx);
}

ClangProject::~ClangProject()
{
    clang_IndexAction_dispose(caction);
    clang_disposeIndex(cidx);
}

bool ClangProject::save() // mutex held
{
    return false;
    // if (!Server::saveFileIds())
    //     return false;
    // Path p = path();
    // Server::encodePath(p);
    // p.prepend(Server::options().dataDir);
    // Path::mkdir(Server::options().dataDir);

    // FILE *f = fopen(p.constData(), "w");
    // if (!f) {
    //     error("Couldn't open %s for writing", p.constData());
    //     return false;
    // }
    // Serializer serializer(f);
    // serializer << static_cast<uint32_t>(0) << static_cast<uint32_t>(Server::DatabaseVersion)
    //            << incs << depends << reverseDepends << names << usrs << decls << defs << refs << virtuals << umap
    //            << static_cast<uint32_t>(units.size());
    // for (Map<uint32_t, ClangUnit*>::const_iterator it = units.begin(); it != units.end(); ++it) {
    //     // probably don't need a mutex here since all threads have finished and
    //     // I hold the project mutex so nothing new should be able to happen
    //     serializer << it->first << it->second->indexed;
    // }

    // const uint32_t size = ftell(f);
    // fseek(f, sizeof(uint32_t), SEEK_SET);
    // serializer << size;
    // fclose(f);
    // return true;
}

bool ClangProject::load()
{
    return false;
    Path p = path();
    Server::encodePath(p);
    p.prepend(Server::options().dataDir);
    FILE *f = fopen(p.constData(), "r");
    if (!f)
        return false;

    Deserializer deserializer(f);
    uint32_t size;
    deserializer >> size;
    if (size != static_cast<uint32_t>(Rct::fileSize(f))) {
        fclose(f);
        error() << p << "seems to be corrupted. Refusing to load";
        return false;
    }
    uint32_t version;
    deserializer >> version;
    if (version != Server::DatabaseVersion) {
        fclose(f);
        return false;
    }
    uint32_t unitCount;
    deserializer >> incs >> depends >> reverseDepends >> names >> usrs >> decls >> defs
                 >> refs >> virtuals >> umap >> unitCount;

    fclose(f);
    return true;
}

Project::Cursor ClangProject::cursor(const Location &location) const
{
    MutexLocker locker(&mutex);
    Map<Location, CursorInfo>::const_iterator usr = usrs.lower_bound(location);
    if (usr == usrs.end())
        return Project::Cursor();
    if (usr->first > location) { // we're looking for the previous one
        if (usr == usrs.begin())
            return Project::Cursor();
        --usr;
        if (usr->first.path() != location.path()) {
            // we've iterated past the beginning of the file
            return Project::Cursor();
        }
        if ((usr->first.line() < location.line())
            || (usr->first.column() + usr->second.length() <= location.column())) {
            // our location is after the start of the the previous location
            return Project::Cursor();
        }
        assert(usr->first.line() == location.line());
    }
    assert(!(usr->first > location));

    //error() << "found loc, asked for" << location << "resolved to" << usr->first
    //        << "refers to" << usr->second.loc << "and kind" << usr->second.kind;

    Project::Cursor cursor;
    cursor.location = usr->first;
    cursor.kind = usr->second.kind;

    const uint32_t targetUsr = usr->second.usr;

    if (cursor.kind == Project::Cursor::Reference) {
        // reference, target should be definition (if possible)
        UsrSet::const_iterator target = defs.find(targetUsr);
        if (target == defs.end()) {
            // try declaration
            target = decls.find(targetUsr);
            if (target != decls.end()) {
                if (!target->second.isEmpty())
                    cursor.target = *target->second.begin();
            }
        } else {
            if (!target->second.isEmpty())
                cursor.target = *target->second.begin();
        }
    } else if (cursor.isDefinition()) {
        // definition, target should be declaration
        const UsrSet::const_iterator target = decls.find(targetUsr);
        if (target != decls.end()) {
            if (!target->second.isEmpty())
                cursor.target = *target->second.begin();
        }
    } else {
        // declaration, taget should be definition
        const UsrSet::const_iterator target = defs.find(targetUsr);
        if (target != defs.end()) {
            if (!target->second.isEmpty())
                cursor.target = *target->second.begin();
        }
    }

    return cursor;
}

void ClangProject::writeReferences(const uint32_t usr, Connection* conn) const
{
    const UsrSet::const_iterator ref = refs.find(usr);
    if (ref != refs.end()) {
        Set<Location>::const_iterator loc = ref->second.begin();
        const Set<Location>::const_iterator end = ref->second.end();
        while (loc != end) {
            conn->write<256>("%s:%d:%d %c\t", loc->path().nullTerminated(), loc->line(), loc->column(), 'r');
            ++loc;
        }
    }
}

void ClangProject::writeDeclarations(const uint32_t usr, Connection* conn) const
{
    const UsrSet* usrs[] = { &decls, &defs, 0 };
    for (int i = 0; usrs[i]; ++i) {
        const UsrSet::const_iterator decl = usrs[i]->find(usr);
        if (decl != usrs[i]->end()) {
            Set<Location>::const_iterator loc = decl->second.begin();
            const Set<Location>::const_iterator end = decl->second.end();
            while (loc != end) {
                conn->write<256>("%s:%d:%d %c\t", loc->path().nullTerminated(), loc->line(), loc->column(), 'r');
                ++loc;
            }
        }
    }
}

void ClangProject::references(const Location& location, unsigned queryFlags,
                               const List<Path> &pathFilter, Connection *conn) const
{
#warning need to respect pathFilter
    const bool wantVirtuals = queryFlags & QueryMessage::FindVirtuals;
    const bool wantAll = queryFlags & QueryMessage::AllReferences;

    MutexLocker locker(&mutex);
    Map<Location, CursorInfo>::const_iterator usr = usrs.lower_bound(location);
    if (usr == usrs.end()) {
        conn->write("`");
        return;
    }
    if (usr->first > location) { // we're looking for the previous one
        if (usr == usrs.begin()) {
            conn->write("`");
            return;
        }
        --usr;
        if (usr->first.path() != location.path()) {
            // we've iterated past the beginning of the file
            conn->write("`");
            return;
        }
        if ((usr->first.line() < location.line())
            || (usr->first.column() + usr->second.length() <= location.column())) {
            // our location is after the start of the the previous location
            conn->write("`");
            return;
        }
        assert(usr->first.line() == location.line());
    }
    assert(!(usr->first > location));

    const uint32_t targetUsr = usr->second.usr;

    if (wantAll || !wantVirtuals) {
        writeReferences(targetUsr, conn);
        if (wantAll)
            writeDeclarations(targetUsr, conn);
    }
    if (wantVirtuals) {
        if (wantAll)
            writeReferences(targetUsr, conn);
        writeDeclarations(targetUsr, conn);

        const VirtualSet::const_iterator virt = virtuals.find(targetUsr);
        Set<uint32_t>::const_iterator vusr = virt->second.begin();
        const Set<uint32_t>::const_iterator vend = virt->second.end();
        while (vusr != vend) {
            if (wantAll)
                writeReferences(*vusr, conn);
            writeDeclarations(*vusr, conn);
            ++vusr;
        }
    }
    conn->write("`");
}

void ClangProject::status(const String &query, Connection *conn) const
{
}

void ClangProject::dump(const SourceInformation &sourceInformation, Connection *conn) const
{
}

int ClangProject::index(const SourceInformation &sourceInformation)
{
    const uint32_t fileId = Location::insertFile(sourceInformation.sourceFile);
    ClangUnit *&unit = units[fileId];
    if (!unit)
        unit = new ClangUnit(this);
    assert(unit);
    {
        MutexLocker locker(&mutex);
        if (!pendingJobs++)
            timer.restart();
        ++jobsProcessed;
    }
    unit->reindex(sourceInformation);
    return -1;
}

void ClangProject::remove(const Path &sourceFile)
{
    const uint32_t fileId = Location::fileId(sourceFile);

    MutexLocker locker(&mutex);
    {
        // remove headers?
        DependSet::iterator dep = depends.find(fileId);
        if (dep != depends.end())
            depends.erase(dep);
    }
    {
        DependSet::iterator dep = reverseDepends.begin();
        while (dep != reverseDepends.end()) {
            Set<uint32_t>& set = dep->second;
            if (set.remove(fileId)) {
                if (set.isEmpty())
                    reverseDepends.erase(dep++);
                else
                    ++dep;
            } else {
                ++dep;
            }
        }
    }
}

bool ClangProject::isIndexing() const
{
    MutexLocker locker(&mutex);
    return (pendingJobs > 0);
}

Set<Path> ClangProject::dependencies(const Path &path, DependencyMode mode) const
{
    MutexLocker locker(&mutex);

    Set<uint32_t> deps;
    const uint32_t fileId = Location::fileId(path);
    if (mode == ArgDependsOn) {
        addDeps(fileId, depends, deps);
    } else {
        addDeps(fileId, reverseDepends, deps);
    }

    Set<Path> result;
    result.insert(path); // all files depend on themselves

    Set<uint32_t>::const_iterator dep = deps.begin();
    const Set<uint32_t>::const_iterator end = deps.end();
    while (dep != end) {
        result.insert(Location::path(*dep));
        ++dep;
    }

    return result;
}

Set<Path> ClangProject::files(int mode) const
{
    return Set<Path>();
}

Set<String> ClangProject::listSymbols(const String &string, const List<Path> &pathFilter) const
{
#warning need to respect pathFilter
    Set<String> result;

    MutexLocker locker(&mutex);
    Map<String, Set<uint32_t> >::const_iterator name = names.lower_bound(string);
    const Map<String, Set<uint32_t> >::const_iterator end = names.end();
    while (name != end && name->first.startsWith(string)) {
        result.insert(name->first);
        ++name;
    }

    return result;
}

static inline Location firstLocation(const uint32_t usr, const UsrSet& set)
{
    const UsrSet::const_iterator it = set.find(usr);
    if (it == set.end())
        return Location();
    const Set<Location>& locs = it->second;
    if (locs.isEmpty())
        return Location();
    return *locs.begin();
}

Set<Project::Cursor> ClangProject::findCursors(const String &string, const List<Path> &pathFilter) const
{
#warning need to respect pathFilter
    MutexLocker locker(&mutex);
    Map<String, Set<uint32_t> >::const_iterator name = names.find(string);
    if (name == names.end())
        return Set<Cursor>();

    Set<Cursor> cursors;

    Set<uint32_t>::const_iterator usr = name->second.begin();
    const Set<uint32_t>::const_iterator end = name->second.end();
    while (usr != end) {
        const UsrSet* usrs[] = { &decls, &defs, 0 };
        for (int i = 0; usrs[i]; ++i) {
            const UsrSet::const_iterator decl = usrs[i]->find(*usr);
            if (decl != usrs[i]->end()) {
                Set<Location>::const_iterator loc = decl->second.begin();
                const Set<Location>::const_iterator end = decl->second.end();
                while (loc != end) {
                    Map<Location, CursorInfo>::const_iterator info = ClangProject::usrs.find(*loc);
                    if (info != ClangProject::usrs.end()) {
                        Cursor cursor;
                        cursor.symbolName = name->first;
                        cursor.location = *loc;
                        cursor.target = firstLocation(*usr, usrs[i] == &decls ? defs : decls);
                        cursor.kind = info->second.kind;
                        cursor.start = info->second.start;
                        cursor.end = info->second.end;
                        cursors.insert(cursor);
                    }
                    ++loc;
                }
            }
        }
        ++usr;
    }

    return cursors;
}

String ClangProject::fixits(const Path &path) const
{
    MutexLocker lock(&mutex);
    const Map<Path, Set<FixIt> >::const_iterator it = fixIts.find(path);
    String out;
    if (it != fixIts.end()) {
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

Set<Project::Cursor> ClangProject::cursors(const Path &path) const
{
#warning implement me
    return Set<Cursor>();
}

bool ClangProject::codeCompleteAt(const Location &location, const String &source, Connection *conn)
{
    shared_ptr<UnitCache::Unit> unit = UnitCache::get(location.path());
    // if (unit && unit->translationUnit) {

    // }

    return false;
}

class ClangProjectPlugin : public RTagsPlugin
{
public:
    virtual shared_ptr<Project> createProject(const Path &path)
    {
        return shared_ptr<Project>(new ClangProject(path));
    }
};

extern "C" RTagsPlugin* createInstance()
{
    return new ClangProjectPlugin;
}
