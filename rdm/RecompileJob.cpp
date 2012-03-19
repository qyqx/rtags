#include "RecompileJob.h"
#include "UnitCache.h"
#include <Rdm.h>
#include "Indexer.h"

RecompileJob::RecompileJob(const QByteArray& fn, int i)
    : fileName(fn), id(i)
{
}

RecompileJob::~RecompileJob()
{
}

void RecompileJob::run()
{
    CachedUnit locker(fileName, UnitCache::Source | UnitCache::Info);
    UnitCache::Unit* data = locker.unit();
    if (data) {
        // everything is good!
        Indexer::instance()->index(fileName, QList<QByteArray>(),
                                   UnitCache::AST|UnitCache::Memory|UnitCache::Info|UnitCache::ForceReindex);
        emit complete(id, QList<QByteArray>());
        return;
    }

    Rdm::FirstUnitData first;
    first.fileName = fileName;
    Rdm::visitIncluderFiles(fileName, Rdm::visitFindFirstUnit, &first, UnitCache::Source | UnitCache::Info);
    if (!first.data) {
        warning("recompile: no unit for %s", fileName.constData());
    } else {
        Indexer::instance()->index(fileName, QList<QByteArray>(),
                                   UnitCache::AST|UnitCache::Memory|UnitCache::Info|UnitCache::ForceReindex);
    }

    emit complete(id, QList<QByteArray>());
}