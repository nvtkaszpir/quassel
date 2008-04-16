/***************************************************************************
 *   Copyright (C) 2005-07 by the Quassel IRC Team                         *
 *   devel@quassel-irc.org                                                 *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) version 3.                                           *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.             *
 ***************************************************************************/

#include "sqlitestorage.h"

#include <QtSql>

#include "network.h"

#include "util.h"

SqliteStorage::SqliteStorage(QObject *parent)
  : AbstractSqlStorage(parent)
{
}

SqliteStorage::~SqliteStorage() {
}

bool SqliteStorage::isAvailable() const {
  if(!QSqlDatabase::isDriverAvailable("QSQLITE")) return false;
  return true;
}

QString SqliteStorage::displayName() const {
  return QString("SQLite");
}

QString SqliteStorage::description() const {
  return tr("SQLite is a file-based database engine that does not require any setup. It is suitable for small and medium-sized "
            "databases that do not require access via network. Use SQLite if your Quassel Core should store its data on the same machine "
            "it is running on, and if you only expect a few users to use your core.");
}

int SqliteStorage::installedSchemaVersion() {
  QSqlQuery query = logDb().exec("SELECT value FROM coreinfo WHERE key = 'schemaversion'");
  if(query.first())
    return query.value(0).toInt();

  // maybe it's really old... (schema version 0)
  query = logDb().exec("SELECT MAX(version) FROM coreinfo");
  if(query.first())
    return query.value(0).toInt();

  return AbstractSqlStorage::installedSchemaVersion();
}

UserId SqliteStorage::addUser(const QString &user, const QString &password) {
  QSqlQuery query(logDb());
  query.prepare(queryString("insert_quasseluser"));
  query.bindValue(":username", user);
  query.bindValue(":password", cryptedPassword(password));
  query.exec();
  if(query.lastError().isValid() && query.lastError().number() == 19) { // user already exists - sadly 19 seems to be the general constraint violation error...
    return 0;
  }

  query.prepare(queryString("select_userid"));
  query.bindValue(":username", user);
  query.exec();
  query.first();
  UserId uid = query.value(0).toInt();
  emit userAdded(uid, user);
  return uid;
}

void SqliteStorage::updateUser(UserId user, const QString &password) {
  QSqlQuery query(logDb());
  query.prepare(queryString("update_userpassword"));
  query.bindValue(":userid", user.toInt());
  query.bindValue(":password", cryptedPassword(password));
  query.exec();
}

void SqliteStorage::renameUser(UserId user, const QString &newName) {
  QSqlQuery query(logDb());
  query.prepare(queryString("update_username"));
  query.bindValue(":userid", user.toInt());
  query.bindValue(":username", newName);
  query.exec();
  emit userRenamed(user, newName);
}

UserId SqliteStorage::validateUser(const QString &user, const QString &password) {
  QSqlQuery query(logDb());
  query.prepare(queryString("select_authuser"));
  query.bindValue(":username", user);
  query.bindValue(":password", cryptedPassword(password));
  query.exec();

  if(query.first()) {
    return query.value(0).toInt();
  } else {
    return 0;
  }
}

void SqliteStorage::delUser(UserId user) {
  QSqlQuery query(logDb());
  query.prepare(queryString("delete_backlog_by_uid"));
  query.bindValue(":userid", user.toInt());
  query.exec();
  
  query.prepare(queryString("delete_buffers_by_uid"));
  query.bindValue(":userid", user.toInt());
  query.exec();
  
  query.prepare(queryString("delete_networks_by_uid"));
  query.bindValue(":userid", user.toInt());
  query.exec();
  
  query.prepare(queryString("delete_quasseluser"));
  query.bindValue(":userid", user.toInt());
  query.exec();
  // I hate the lack of foreign keys and on delete cascade... :(
  emit userRemoved(user);
}

void SqliteStorage::setUserSetting(UserId userId, const QString &settingName, const QVariant &data) {
  QByteArray rawData;
  QDataStream out(&rawData, QIODevice::WriteOnly);
  out.setVersion(QDataStream::Qt_4_2);
  out << data;

  QSqlQuery query(logDb());
  query.prepare(queryString("insert_user_setting"));
  query.bindValue(":userid", userId.toInt());
  query.bindValue(":settingname", settingName);
  query.bindValue(":settingvalue", rawData);
  query.exec();

  if(query.lastError().isValid()) {
    QSqlQuery updateQuery(logDb());
    updateQuery.prepare(queryString("update_user_setting"));
    updateQuery.bindValue(":userid", userId.toInt());
    updateQuery.bindValue(":settingname", settingName);
    updateQuery.bindValue(":settingvalue", rawData);
    updateQuery.exec();
  }
		  
}

QVariant SqliteStorage::getUserSetting(UserId userId, const QString &settingName, const QVariant &defaultData) {
  QSqlQuery query(logDb());
  query.prepare(queryString("select_user_setting"));
  query.bindValue(":userid", userId.toInt());
  query.bindValue(":settingname", settingName);
  query.exec();

  if(query.first()) {
    QVariant data;
    QByteArray rawData = query.value(0).toByteArray();
    QDataStream in(&rawData, QIODevice::ReadOnly);
    in.setVersion(QDataStream::Qt_4_2);
    in >> data;
    return data;
  } else {
    return defaultData;
  }
}

NetworkId SqliteStorage::createNetwork(UserId user, const NetworkInfo &info) {
  NetworkId networkId;
  QSqlQuery query(logDb());
  query.prepare(queryString("insert_network"));
  query.bindValue(":userid", user.toInt());
  query.bindValue(":networkname", info.networkName);
  query.exec();
  
  networkId = getNetworkId(user, info.networkName);
  if(!networkId.isValid()) {
    watchQuery(&query);
  } else {
    NetworkInfo newNetworkInfo = info;
    newNetworkInfo.networkId = networkId;
    updateNetwork(user, newNetworkInfo);
  }
  return networkId;
}

bool SqliteStorage::updateNetwork(UserId user, const NetworkInfo &info) {
  if(!isValidNetwork(user, info.networkId))
     return false;
  
  QSqlQuery updateQuery(logDb());
  updateQuery.prepare(queryString("update_network"));
  updateQuery.bindValue(":networkname", info.networkName);
  updateQuery.bindValue(":identityid", info.identity.toInt());
  updateQuery.bindValue(":usecustomencoding", info.useCustomEncodings ? 1 : 0);
  updateQuery.bindValue(":encodingcodec", QString(info.codecForEncoding));
  updateQuery.bindValue(":decodingcodec", QString(info.codecForDecoding));
  updateQuery.bindValue(":servercodec", QString(info.codecForServer));
  updateQuery.bindValue(":userandomserver", info.useRandomServer ? 1 : 0);
  updateQuery.bindValue(":perform", info.perform.join("\n"));
  updateQuery.bindValue(":useautoidentify", info.useAutoIdentify ? 1 : 0);
  updateQuery.bindValue(":autoidentifyservice", info.autoIdentifyService);
  updateQuery.bindValue(":autoidentifypassword", info.autoIdentifyPassword);
  updateQuery.bindValue(":useautoreconnect", info.useAutoReconnect ? 1 : 0);
  updateQuery.bindValue(":autoreconnectinterval", info.autoReconnectInterval);
  updateQuery.bindValue(":autoreconnectretries", info.autoReconnectRetries);
  updateQuery.bindValue(":unlimitedconnectretries", info.unlimitedReconnectRetries ? 1 : 0);
  updateQuery.bindValue(":rejoinchannels", info.rejoinChannels ? 1 : 0);
  updateQuery.bindValue(":networkid", info.networkId.toInt());
  updateQuery.exec();
  if(!watchQuery(&updateQuery))
    return false;

  QSqlQuery dropServersQuery(logDb());
  dropServersQuery.prepare("DELETE FROM ircserver WHERE networkid = :networkid");
  dropServersQuery.bindValue(":networkid", info.networkId.toInt());
  dropServersQuery.exec();
  if(!watchQuery(&dropServersQuery))
    return false;

  QSqlQuery insertServersQuery(logDb());
  insertServersQuery.prepare(queryString("insert_server"));
  foreach(QVariant server_, info.serverList) {
    QVariantMap server = server_.toMap();
    insertServersQuery.bindValue(":hostname", server["Host"]);
    insertServersQuery.bindValue(":port", server["Port"].toInt());
    insertServersQuery.bindValue(":password", server["Password"]);
    insertServersQuery.bindValue(":ssl", server["UseSSL"].toBool() ? 1 : 0);
    insertServersQuery.bindValue(":userid", user.toInt());
    insertServersQuery.bindValue(":networkid", info.networkId.toInt());

    insertServersQuery.exec();
    if(!watchQuery(&insertServersQuery))
      return false;
  }
  
  return true;
}

bool SqliteStorage::removeNetwork(UserId user, const NetworkId &networkId) {
  if(!isValidNetwork(user, networkId))
     return false;

  bool withTransaction = logDb().driver()->hasFeature(QSqlDriver::Transactions);
  if(withTransaction) {
    sync();
    if(!logDb().transaction()) {
      qWarning() << "SqliteStorage::removeNetwork(): cannot start transaction. continuing with out rollback support!";
      withTransaction = false;
    }
  }
  
  QSqlQuery deleteBacklogQuery(logDb());
  deleteBacklogQuery.prepare(queryString("delete_backlog_for_network"));
  deleteBacklogQuery.bindValue(":networkid", networkId.toInt());
  deleteBacklogQuery.exec();
  if(!watchQuery(&deleteBacklogQuery)) {
    if(withTransaction)
      logDb().rollback();
    return false;
  }
  
  QSqlQuery deleteBuffersQuery(logDb());
  deleteBuffersQuery.prepare(queryString("delete_buffers_for_network"));
  deleteBuffersQuery.bindValue(":networkid", networkId.toInt());
  deleteBuffersQuery.exec();
  if(!watchQuery(&deleteBuffersQuery)) {
    if(withTransaction)
      logDb().rollback();
    return false;
  }
  
  QSqlQuery deleteServersQuery(logDb());
  deleteServersQuery.prepare(queryString("delete_ircservers_for_network"));
  deleteServersQuery.bindValue(":networkid", networkId.toInt());
  deleteServersQuery.exec();
  if(!watchQuery(&deleteServersQuery)) {
    if(withTransaction)
      logDb().rollback();
    return false;
  }
  
  QSqlQuery deleteNetworkQuery(logDb());
  deleteNetworkQuery.prepare(queryString("delete_network"));
  deleteNetworkQuery.bindValue(":networkid", networkId.toInt());
  deleteNetworkQuery.exec();
  if(!watchQuery(&deleteNetworkQuery)) {
    if(withTransaction)
      logDb().rollback();
    return false;
  }

  logDb().commit();
  return true;
}

QList<NetworkInfo> SqliteStorage::networks(UserId user) {
  QList<NetworkInfo> nets;

  QSqlQuery networksQuery(logDb());
  networksQuery.prepare(queryString("select_networks_for_user"));
  networksQuery.bindValue(":userid", user.toInt());
  
  QSqlQuery serversQuery(logDb());
  serversQuery.prepare(queryString("select_servers_for_network"));

  networksQuery.exec();
  if(!watchQuery(&networksQuery))
    return nets;

  while(networksQuery.next()) {
    NetworkInfo net;
    net.networkId = networksQuery.value(0).toInt();
    net.networkName = networksQuery.value(1).toString();
    net.identity = networksQuery.value(2).toInt();
    net.codecForServer = networksQuery.value(3).toString().toAscii();
    net.codecForEncoding = networksQuery.value(4).toString().toAscii();
    net.codecForDecoding = networksQuery.value(5).toString().toAscii();
    net.useRandomServer = networksQuery.value(6).toInt() == 1 ? true : false;
    net.perform = networksQuery.value(7).toString().split("\n");
    net.useAutoIdentify = networksQuery.value(8).toInt() == 1 ? true : false;
    net.autoIdentifyService = networksQuery.value(9).toString();
    net.autoIdentifyPassword = networksQuery.value(10).toString();
    net.useAutoReconnect = networksQuery.value(11).toInt() == 1 ? true : false;
    net.autoReconnectInterval = networksQuery.value(12).toUInt();
    net.autoReconnectRetries = networksQuery.value(13).toInt();
    net.unlimitedReconnectRetries = networksQuery.value(14).toInt() == 1 ? true : false;
    net.rejoinChannels = networksQuery.value(15).toInt() == 1 ? true : false;

    serversQuery.bindValue(":networkid", net.networkId.toInt());
    serversQuery.exec();
    if(!watchQuery(&serversQuery))
      return nets;

    QVariantList servers;
    while(serversQuery.next()) {
      QVariantMap server;
      server["Host"] = serversQuery.value(0).toString();
      server["Port"] = serversQuery.value(1).toInt();
      server["Password"] = serversQuery.value(2).toString();
      server["UseSSL"] = serversQuery.value(3).toInt() == 1 ? true : false;
      servers << server;
    }
    net.serverList = servers;
    nets << net;
  }
  return nets;
}

bool SqliteStorage::isValidNetwork(UserId user, const NetworkId &networkId) {
  QSqlQuery query(logDb());
  query.prepare(queryString("select_networkExists"));
  query.bindValue(":userid", user.toInt());
  query.bindValue(":networkid", networkId.toInt());
  query.exec();

  watchQuery(&query); // there should not occur any errors
  if(!query.first())
    return false;
  
  Q_ASSERT(!query.next());
  return true;
}

bool SqliteStorage::isValidBuffer(const UserId &user, const BufferId &bufferId) {
  QSqlQuery query(logDb());
  query.prepare(queryString("select_bufferExists"));
  query.bindValue(":userid", user.toInt());
  query.bindValue(":bufferid", bufferId.toInt());
  query.exec();

  watchQuery(&query);
  if(!query.first())
    return false;

  Q_ASSERT(!query.next());
  return true;
}

NetworkId SqliteStorage::getNetworkId(UserId user, const QString &network) {
  QSqlQuery query(logDb());
  query.prepare("SELECT networkid FROM network "
		"WHERE userid = :userid AND networkname = :networkname");
  query.bindValue(":userid", user.toInt());
  query.bindValue(":networkname", network);
  query.exec();
  
  if(query.first())
    return query.value(0).toInt();
  else
    return NetworkId();
}

QList<NetworkId> SqliteStorage::connectedNetworks(UserId user) {
  QList<NetworkId> connectedNets;
  QSqlQuery query(logDb());
  query.prepare(queryString("select_connected_networks"));
  query.bindValue(":userid", user.toInt());
  query.exec();
  watchQuery(&query);

  while(query.next()) {
    connectedNets << query.value(0).toInt();
  }
  
  return connectedNets;
}

void SqliteStorage::setNetworkConnected(UserId user, const NetworkId &networkId, bool isConnected) {
  QSqlQuery query(logDb());
  query.prepare(queryString("update_network_connected"));
  query.bindValue(":userid", user.toInt());
  query.bindValue(":networkid", networkId.toInt());
  query.bindValue(":connected", isConnected ? 1 : 0);
  query.exec();
  watchQuery(&query);
}

QHash<QString, QString> SqliteStorage::persistentChannels(UserId user, const NetworkId &networkId) {
  QHash<QString, QString> persistentChans;
  QSqlQuery query(logDb());
  query.prepare(queryString("select_persistent_channels"));
  query.bindValue(":userid", user.toInt());
  query.bindValue(":networkid", networkId.toInt());
  query.exec();
  watchQuery(&query);

  while(query.next()) {
    persistentChans[query.value(0).toString()] = query.value(1).toString();
  }
  
  return persistentChans;
}

void SqliteStorage::setChannelPersistent(UserId user, const NetworkId &networkId, const QString &channel, bool isJoined) {
  QSqlQuery query(logDb());
  query.prepare(queryString("update_buffer_persistent_channel"));
  query.bindValue(":userid", user.toInt());
  query.bindValue(":networkId", networkId.toInt());
  query.bindValue(":buffercname", channel.toLower());
  query.bindValue(":joined", isJoined ? 1 : 0);
  query.exec();
  watchQuery(&query);
}

void SqliteStorage::setPersistentChannelKey(UserId user, const NetworkId &networkId, const QString &channel, const QString &key) {
  QSqlQuery query(logDb());
  query.prepare(queryString("update_buffer_set_channel_key"));
  query.bindValue(":userid", user.toInt());
  query.bindValue(":networkId", networkId.toInt());
  query.bindValue(":buffercname", channel.toLower());
  query.bindValue(":key", key);
  query.exec();
  watchQuery(&query);
}


void SqliteStorage::createBuffer(UserId user, const NetworkId &networkId, BufferInfo::Type type, const QString &buffer) {
  QSqlQuery *query = cachedQuery("insert_buffer");
  query->bindValue(":userid", user.toInt());
  query->bindValue(":networkid", networkId.toInt());
  query->bindValue(":buffertype", (int)type);
  query->bindValue(":buffername", buffer);
  query->bindValue(":buffercname", buffer.toLower());
  query->exec();

  watchQuery(query);
}

BufferInfo SqliteStorage::getBufferInfo(UserId user, const NetworkId &networkId, BufferInfo::Type type, const QString &buffer) {
  QSqlQuery *query = cachedQuery("select_bufferByName");
  query->bindValue(":networkid", networkId.toInt());
  query->bindValue(":userid", user.toInt());
  query->bindValue(":buffercname", buffer.toLower());
  query->exec();

  if(!query->first()) {
    createBuffer(user, networkId, type, buffer);
    query->exec();
    if(!query->first()) {
      watchQuery(query);
      qWarning() << "unable to create BufferInfo for:" << user << networkId << buffer;
      return BufferInfo();
    }
  }

  BufferInfo bufferInfo = BufferInfo(query->value(0).toInt(), networkId, (BufferInfo::Type)query->value(1).toInt(), 0, buffer);
  if(query->next()) {
    qWarning() << "SqliteStorage::getBufferInfo(): received more then one Buffer!";
    qWarning() << "         Query:" << query->lastQuery();
    qWarning() << "  bound Values:" << query->boundValues();
    Q_ASSERT(false);
  }

  return bufferInfo;
}

BufferInfo SqliteStorage::getBufferInfo(UserId user, const BufferId &bufferId) {
  QSqlQuery query(logDb());
  query.prepare(queryString("select_buffer_by_id"));
  query.bindValue(":userid", user.toInt());
  query.bindValue(":bufferid", bufferId.toInt());
  query.exec();
  if(!watchQuery(&query))
    return BufferInfo();

  if(!query.first())
    return BufferInfo();

  BufferInfo bufferInfo(query.value(0).toInt(), query.value(1).toInt(), (BufferInfo::Type)query.value(2).toInt(), 0, query.value(4).toString());
  Q_ASSERT(!query.next());

  return bufferInfo;
}

QList<BufferInfo> SqliteStorage::requestBuffers(UserId user) {
  QList<BufferInfo> bufferlist;
  QSqlQuery query(logDb());
  query.prepare(queryString("select_buffers"));
  query.bindValue(":userid", user.toInt());
  
  query.exec();
  watchQuery(&query);
  while(query.next()) {
    bufferlist << BufferInfo(query.value(0).toInt(), query.value(1).toInt(), (BufferInfo::Type)query.value(2).toInt(), query.value(3).toInt(), query.value(4).toString());
  }
  return bufferlist;
}

bool SqliteStorage::removeBuffer(const UserId &user, const BufferId &bufferId) {
  if(!isValidBuffer(user, bufferId))
    return false;

  QSqlQuery delBacklogQuery(logDb());
  delBacklogQuery.prepare(queryString("delete_backlog_for_buffer"));
  delBacklogQuery.bindValue(":bufferid", bufferId.toInt());
  delBacklogQuery.exec();
  if(!watchQuery(&delBacklogQuery))
    return false;

  QSqlQuery delBufferQuery(logDb());
  delBufferQuery.prepare(queryString("delete_buffer_for_bufferid"));
  delBufferQuery.bindValue(":bufferid", bufferId.toInt());
  delBufferQuery.exec();
  if(!watchQuery(&delBufferQuery))
    return false;

  return true;
}

BufferId SqliteStorage::renameBuffer(const UserId &user, const NetworkId &networkId, const QString &newName, const QString &oldName) {
  // check if such a buffer exists...
  QSqlQuery existsQuery(logDb());
  existsQuery.prepare(queryString("select_bufferByName"));
  existsQuery.bindValue(":networkid", networkId.toInt());
  existsQuery.bindValue(":userid", user.toInt());
  existsQuery.bindValue(":buffercname", oldName.toLower());
  existsQuery.exec();
  if(!watchQuery(&existsQuery))
    return false;

  if(!existsQuery.first())
    return false;

  const int bufferid = existsQuery.value(0).toInt();

  Q_ASSERT(!existsQuery.next());

  // ... and if the new name is still free.
  existsQuery.bindValue(":networkid", networkId.toInt());
  existsQuery.bindValue(":userid", user.toInt());
  existsQuery.bindValue(":buffercname", newName.toLower());
  existsQuery.exec();
  if(!watchQuery(&existsQuery))
    return false;

  if(existsQuery.first())
    return false;

  QSqlQuery renameBufferQuery(logDb());
  renameBufferQuery.prepare(queryString("update_buffer_name"));
  renameBufferQuery.bindValue(":buffername", newName);
  renameBufferQuery.bindValue(":buffercname", newName.toLower());
  renameBufferQuery.bindValue(":bufferid", bufferid);
  renameBufferQuery.exec();
  if(watchQuery(&existsQuery))
    return BufferId(bufferid);
  else
    return BufferId();
}

void SqliteStorage::setBufferLastSeenMsg(UserId user, const BufferId &bufferId, const MsgId &msgId) {
  QSqlQuery *query = cachedQuery("update_buffer_lastseen");
  query->bindValue(":userid", user.toInt());
  query->bindValue(":bufferid", bufferId.toInt());
  query->bindValue(":lastseenmsgid", msgId.toInt());
  query->exec();
  watchQuery(query);
}

QHash<BufferId, MsgId> SqliteStorage::bufferLastSeenMsgIds(UserId user) {
  QHash<BufferId, MsgId> lastSeenHash;
  QSqlQuery query(logDb());
  query.prepare(queryString("select_buffer_lastseen_messages"));
  query.bindValue(":userid", user.toInt());
  query.exec();
  if(!watchQuery(&query))
    return lastSeenHash;

  while(query.next()) {
    lastSeenHash[query.value(0).toInt()] = query.value(1).toInt();
  }
  return lastSeenHash;
}

MsgId SqliteStorage::logMessage(Message msg) {
  QSqlQuery *logMessageQuery = cachedQuery("insert_message");
  logMessageQuery->bindValue(":time", msg.timestamp().toTime_t());
  logMessageQuery->bindValue(":bufferid", msg.bufferInfo().bufferId().toInt());
  logMessageQuery->bindValue(":type", msg.type());
  logMessageQuery->bindValue(":flags", msg.flags());
  logMessageQuery->bindValue(":sender", msg.sender());
  logMessageQuery->bindValue(":message", msg.text());
  logMessageQuery->exec();
  
  if(logMessageQuery->lastError().isValid()) {
    // constraint violation - must be NOT NULL constraint - probably the sender is missing...
    if(logMessageQuery->lastError().number() == 19) {
      QSqlQuery *addSenderQuery = cachedQuery("insert_sender");
      addSenderQuery->bindValue(":sender", msg.sender());
      addSenderQuery->exec();
      watchQuery(addSenderQuery);
      logMessageQuery->exec();
      if(!watchQuery(logMessageQuery))
	return 0;
    } else {
      watchQuery(logMessageQuery);
    }
  }

  MsgId msgId = logMessageQuery->lastInsertId().toInt();
  Q_ASSERT(msgId.isValid());
  return msgId;
}

QList<Message> SqliteStorage::requestMsgs(UserId user, BufferId bufferId, int lastmsgs, int offset) {
  QList<Message> messagelist;

  BufferInfo bufferInfo = getBufferInfo(user, bufferId);
  if(!bufferInfo.isValid())
    return messagelist;

  if(offset == -1) {
    offset = 0;
  } else {
    // we have to determine the real offset first
    QSqlQuery *offsetQuery = cachedQuery("select_messagesOffset");
    offsetQuery->bindValue(":bufferid", bufferId.toInt());
    offsetQuery->bindValue(":messageid", offset);
    offsetQuery->exec();
    offsetQuery->first();
    offset = offsetQuery->value(0).toInt();
  }

  // now let's select the messages
  QSqlQuery *msgQuery = cachedQuery("select_messages");
  msgQuery->bindValue(":bufferid", bufferId.toInt());
  msgQuery->bindValue(":limit", lastmsgs);
  msgQuery->bindValue(":offset", offset);
  msgQuery->exec();
  
  watchQuery(msgQuery);
  
  while(msgQuery->next()) {
    Message msg(QDateTime::fromTime_t(msgQuery->value(1).toInt()),
                bufferInfo,
                (Message::Type)msgQuery->value(2).toUInt(),
                msgQuery->value(5).toString(),
                msgQuery->value(4).toString(),
                msgQuery->value(3).toUInt());
    msg.setMsgId(msgQuery->value(0).toInt());
    messagelist << msg;
  }
  return messagelist;
}


QList<Message> SqliteStorage::requestMsgs(UserId user, BufferId bufferId, QDateTime since, int offset) {
  QList<Message> messagelist;

  BufferInfo bufferInfo = getBufferInfo(user, bufferId);
  if(!bufferInfo.isValid())
    return messagelist;

  // we have to determine the real offset first
  QSqlQuery *offsetQuery = cachedQuery("select_messagesSinceOffset");
  offsetQuery->bindValue(":bufferid", bufferId.toInt());
  offsetQuery->bindValue(":since", since.toTime_t());
  offsetQuery->exec();
  offsetQuery->first();
  offset = offsetQuery->value(0).toInt();

  // now let's select the messages
  QSqlQuery *msgQuery = cachedQuery("select_messagesSince");
  msgQuery->bindValue(":bufferid", bufferId.toInt());
  msgQuery->bindValue(":since", since.toTime_t());
  msgQuery->bindValue(":offset", offset);
  msgQuery->exec();

  watchQuery(msgQuery);
  
  while(msgQuery->next()) {
    Message msg(QDateTime::fromTime_t(msgQuery->value(1).toInt()),
                bufferInfo,
                (Message::Type)msgQuery->value(2).toUInt(),
                msgQuery->value(5).toString(),
                msgQuery->value(4).toString(),
                msgQuery->value(3).toUInt());
    msg.setMsgId(msgQuery->value(0).toInt());
    messagelist << msg;
  }

  return messagelist;
}


QList<Message> SqliteStorage::requestMsgRange(UserId user, BufferId bufferId, int first, int last) {
  QList<Message> messagelist;

  BufferInfo bufferInfo = getBufferInfo(user, bufferId);
  if(!bufferInfo.isValid())
    return messagelist;

  QSqlQuery *rangeQuery = cachedQuery("select_messageRange");
  rangeQuery->bindValue(":bufferid", bufferId.toInt());
  rangeQuery->bindValue(":firstmsg", first);
  rangeQuery->bindValue(":lastmsg", last);
  rangeQuery->exec();

  watchQuery(rangeQuery);
  
  while(rangeQuery->next()) {
    Message msg(QDateTime::fromTime_t(rangeQuery->value(1).toInt()),
                bufferInfo,
                (Message::Type)rangeQuery->value(2).toUInt(),
                rangeQuery->value(5).toString(),
                rangeQuery->value(4).toString(),
                rangeQuery->value(3).toUInt());
    msg.setMsgId(rangeQuery->value(0).toInt());
    messagelist << msg;
  }

  return messagelist;
}

QString SqliteStorage::backlogFile() {
  return quasselDir().absolutePath() + "/quassel-storage.sqlite";  
}


// ONLY NEEDED FOR MIGRATION
bool SqliteStorage::init(const QVariantMap &settings) {
  if(!AbstractSqlStorage::init(settings))
    return false;

  QSqlQuery checkMigratedQuery(logDb());
  checkMigratedQuery.prepare("SELECT DISTINCT typeOf(password) FROM quasseluser");
  checkMigratedQuery.exec();
  if(!watchQuery(&checkMigratedQuery))
    return false;

  if(!checkMigratedQuery.first())
    return true;		// table is empty -> no work to be done

  QString passType = checkMigratedQuery.value(0).toString().toLower();
  if(passType == "text")
    return true; // allready migrated

  Q_ASSERT(passType == "blob");
  
  QSqlQuery getPasswordsQuery(logDb());
  getPasswordsQuery.prepare("SELECT userid, password FROM quasseluser");
  getPasswordsQuery.exec();

  if(!watchQuery(&getPasswordsQuery)) {
    qWarning() << "unable to migrate to new password format!";
    return false;
  }

  QHash<int, QByteArray> passHash;
  while(getPasswordsQuery.next()) {
    passHash[getPasswordsQuery.value(0).toInt()] = getPasswordsQuery.value(1).toByteArray();
  }

  QSqlQuery setPasswordsQuery(logDb());
  setPasswordsQuery.prepare("UPDATE quasseluser SET password = :password WHERE userid = :userid");
  foreach(int userId, passHash.keys()) {
    setPasswordsQuery.bindValue(":password", QString(passHash[userId]));
    setPasswordsQuery.bindValue(":userid", userId);
    setPasswordsQuery.exec();
    watchQuery(&setPasswordsQuery);
  }

  qDebug() << "successfully migrated passwords!";
  return true;
}
