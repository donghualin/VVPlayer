#ifndef LIBPLAYERENGINE_GLOBAL_H
#define LIBPLAYERENGINE_GLOBAL_H

#include <QtCore/qglobal.h>

#if defined(_MSC_VER) || defined(WIN64) || defined(_WIN64) || defined(_WIN32)
#  if defined(LIBPLAYERENGINE_LIBRARY)
#    define LIBPLAYERENGINESHARED_EXPORT Q_DECL_EXPORT
#  else
#    define LIBPLAYERENGINESHARED_EXPORT Q_DECL_IMPORT
#  endif
#else
#  define LIBPLAYERENGINESHARED_EXPORT
#endif

#endif // LIBPLAYERENGINE_GLOBAL_H
