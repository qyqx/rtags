#ifndef CursorInfo_h
#define CursorInfo_h

#include <QByteArray>
#include <clang-c/Index.h>
#include <Path.h>
#include <QDebug>
#include <RTags.h>
#include "Location.h"
#include "Rdm.h"

class CursorInfo
{
public:
    CursorInfo()
        : symbolLength(0), kind(CXCursor_FirstInvalid)
    {}
    bool isNull() const { return !symbolLength; }
    void clear()
    {
        symbolLength = 0;
        kind = CXCursor_FirstInvalid;
        target.clear();
        references.clear();
        symbolName.clear();
    }

    bool dirty(const QSet<quint32> &fileIds)
    {
        bool changed = false;
        if (fileIds.contains(target.fileId())) {
            changed = true;
            target.clear();
        }

        QSet<Location>::iterator it = references.begin();
        while (it != references.end()) {
            if (fileIds.contains((*it).fileId())) {
                changed = true;
                it = references.erase(it);
            } else {
                ++it;
            }
        }
        return changed;
    }

    bool unite(const CursorInfo &other)
    {
        bool changed = false;
        if (target.isNull() && !other.target.isNull()) {
#ifdef QT_DEBUG
            if (!target.isNull()) {
                switch (kind) {
                case CXCursor_TypeRef:
                case CXCursor_NamespaceRef:
                case CXCursor_MacroExpansion:
                case CXCursor_TemplateRef:
                case CXCursor_CXXBaseSpecifier:
                case CXCursor_UnexposedExpr:
                case CXCursor_CallExpr: // don't like this one
                    break;
                case CXCursor_VarDecl:
                case CXCursor_CXXMethod:
                    if (target.path().contains("moc_") || target.path().contains(".moc"))
                        break;
                    // fallthrough
                default:
                    warning() << "overwrote target from" << target << "to" << other.target
                              << "symbolName" << symbolName
                              << Rdm::eatString(clang_getCursorKindSpelling(kind));
                    break;
                }
            }
#endif
            target = other.target;
            changed = true;
        }

        // ### this is not ideal, we can probably know this rather than check all of them
        if (symbolName.isEmpty() && !other.symbolName.isEmpty()) {
            symbolName = other.symbolName;
            changed = true;
        }

        if (kind == CXCursor_FirstInvalid && other.kind != CXCursor_FirstInvalid) {
            kind = other.kind;
            changed = true;
        }

        if (!symbolLength && other.symbolLength) {
            symbolLength = other.symbolLength;
            changed = true;
        }
        const int oldSize = references.size();
        if (!oldSize) {
            references = other.references;
            if (!other.references.isEmpty())
                changed = true;
        } else {
            references.unite(other.references);
            if (oldSize != references.size())
                changed = true;
        }
        return changed;
    }


    int symbolLength; // this is just the symbol name e.g. foo
    QByteArray symbolName; // this is fully qualified Foobar::Barfoo::foo
    CXCursorKind kind;
    Location target;
    QSet<Location> references;
};

#endif