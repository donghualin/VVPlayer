#ifndef IPLAYERENGINE_H
#define IPLAYERENGINE_H

#include "libPlayerEngine_global.h"
#include <QString>

class LIBPLAYERENGINESHARED_EXPORT IPlayerEngine
{
public:
    IPlayerEngine();
    virtual ~IPlayerEngine();

    virtual bool open(const QString& filePath) = 0;
    virtual bool play() = 0;
    virtual bool pause() = 0;
    virtual bool stop() = 0;
    virtual bool isPlaying() const = 0;
};

#endif // IPLAYERENGINE_H