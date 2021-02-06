/*
 * Copyright 2019 Richard Liebscher <richard.liebscher@gmail.com>.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "humanize.h"

#include <QQmlEngine>
#include <QtQml>

//#include <sailfishconnect/helper/humanize.h>

namespace QmlJs {

Humanize::Humanize(QObject *parent) : QObject(parent)
{

}

QString Humanize::bytes(qint64 bytes)
{
    // return humanizeBytes(bytes);
    return QString();
}

void Humanize::registerType()
{
    qmlRegisterSingletonType<Humanize>(
                "SailfishConnect.Qml", 0, 3, "Humanize",
                [](QQmlEngine*, QJSEngine*) -> QObject* {
        return new Humanize();
    });
}

} // namespace QmlJs
