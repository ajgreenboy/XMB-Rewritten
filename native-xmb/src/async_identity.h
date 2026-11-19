#pragma once
#include <QtGlobal>

namespace xmb {
inline bool asyncIdentityMatches(quint64 currentPage,quint64 callbackPage,
                                 quint64 currentRequest,quint64 callbackRequest,
                                 quint64 currentTile,quint64 callbackTile) {
    return currentPage==callbackPage && currentRequest==callbackRequest && currentTile==callbackTile;
}
}
