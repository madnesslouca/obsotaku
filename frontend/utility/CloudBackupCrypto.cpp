/******************************************************************************
    Copyright (C) 2026

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#include "CloudBackupCrypto.hpp"

#include <QObject>
#include <QRandomGenerator>

#ifdef _WIN32
#include <mbedtls/gcm.h>
#include <mbedtls/pkcs5.h>
#endif

#include <algorithm>
#include <cstring>

namespace {
constexpr int SALT_BYTES = 16;
constexpr int NONCE_BYTES = 12;
constexpr int TAG_BYTES = 16;
constexpr int KEY_BYTES = 32;
constexpr unsigned int PBKDF2_ITERATIONS = 250000;

QByteArray RandomBytes(int count)
{
	QByteArray value(count, Qt::Uninitialized);
	for (int offset = 0; offset < count; offset += 4) {
		const quint32 random = QRandomGenerator::system()->generate();
		const int copy = std::min(4, count - offset);
		memcpy(value.data() + offset, &random, static_cast<size_t>(copy));
	}
	return value;
}

void ClearKey(unsigned char *key)
{
	volatile unsigned char *bytes = key;
	for (int index = 0; index < KEY_BYTES; ++index)
		bytes[index] = 0;
}

#ifdef _WIN32
bool DeriveKey(const QString &passphrase, const QByteArray &salt, unsigned char *key, QString &error)
{
	const QByteArray password = passphrase.toUtf8();
	const int result = mbedtls_pkcs5_pbkdf2_hmac_ext(
		MBEDTLS_MD_SHA256, reinterpret_cast<const unsigned char *>(password.constData()), password.size(),
		reinterpret_cast<const unsigned char *>(salt.constData()), salt.size(), PBKDF2_ITERATIONS, KEY_BYTES, key);
	if (result != 0) {
		error = QObject::tr("Não foi possível derivar a chave de criptografia (%1).").arg(result);
		return false;
	}
	return true;
}
#endif
} // namespace

bool CloudBackupCrypto::Supported()
{
#ifdef _WIN32
	return true;
#else
	return false;
#endif
}

bool CloudBackupCrypto::Encrypt(const QByteArray &plain, const QString &passphrase, QByteArray &encrypted,
				QString &error)
{
#ifdef _WIN32
	if (passphrase.isEmpty()) {
		error = QObject::tr("Defina uma senha para criptografar o backup.");
		return false;
	}
	const QByteArray salt = RandomBytes(SALT_BYTES);
	const QByteArray nonce = RandomBytes(NONCE_BYTES);
	unsigned char key[KEY_BYTES]{};
	if (!DeriveKey(passphrase, salt, key, error))
		return false;
	QByteArray cipher(plain.size(), Qt::Uninitialized);
	QByteArray tag(TAG_BYTES, Qt::Uninitialized);
	mbedtls_gcm_context context;
	mbedtls_gcm_init(&context);
	int result = mbedtls_gcm_setkey(&context, MBEDTLS_CIPHER_ID_AES, key, KEY_BYTES * 8);
	if (result == 0) {
		result = mbedtls_gcm_crypt_and_tag(
			&context, MBEDTLS_GCM_ENCRYPT, plain.size(),
			reinterpret_cast<const unsigned char *>(nonce.constData()), nonce.size(), nullptr, 0,
			reinterpret_cast<const unsigned char *>(plain.constData()),
			reinterpret_cast<unsigned char *>(cipher.data()), tag.size(),
			reinterpret_cast<unsigned char *>(tag.data()));
	}
	mbedtls_gcm_free(&context);
	ClearKey(key);
	if (result != 0) {
		error = QObject::tr("Falha ao criptografar o backup (%1).").arg(result);
		return false;
	}
	encrypted = salt + nonce + tag + cipher;
	error.clear();
	return true;
#else
	Q_UNUSED(plain);
	Q_UNUSED(passphrase);
	Q_UNUSED(encrypted);
	error = QObject::tr("A criptografia de backup não está disponível nesta plataforma.");
	return false;
#endif
}

bool CloudBackupCrypto::Decrypt(const QByteArray &encrypted, const QString &passphrase, QByteArray &plain,
				QString &error)
{
#ifdef _WIN32
	if (encrypted.size() < SALT_BYTES + NONCE_BYTES + TAG_BYTES) {
		error = QObject::tr("O backup criptografado está incompleto.");
		return false;
	}
	const QByteArray salt = encrypted.left(SALT_BYTES);
	const QByteArray nonce = encrypted.mid(SALT_BYTES, NONCE_BYTES);
	const QByteArray tag = encrypted.mid(SALT_BYTES + NONCE_BYTES, TAG_BYTES);
	const QByteArray cipher = encrypted.mid(SALT_BYTES + NONCE_BYTES + TAG_BYTES);
	unsigned char key[KEY_BYTES]{};
	if (!DeriveKey(passphrase, salt, key, error))
		return false;
	plain.resize(cipher.size());
	mbedtls_gcm_context context;
	mbedtls_gcm_init(&context);
	int result = mbedtls_gcm_setkey(&context, MBEDTLS_CIPHER_ID_AES, key, KEY_BYTES * 8);
	if (result == 0) {
		result = mbedtls_gcm_auth_decrypt(
			&context, cipher.size(), reinterpret_cast<const unsigned char *>(nonce.constData()), nonce.size(),
			nullptr, 0, reinterpret_cast<const unsigned char *>(tag.constData()), tag.size(),
			reinterpret_cast<const unsigned char *>(cipher.constData()),
			reinterpret_cast<unsigned char *>(plain.data()));
	}
	mbedtls_gcm_free(&context);
	ClearKey(key);
	if (result != 0) {
		plain.clear();
		error = QObject::tr("Senha incorreta ou backup corrompido.");
		return false;
	}
	error.clear();
	return true;
#else
	Q_UNUSED(encrypted);
	Q_UNUSED(passphrase);
	Q_UNUSED(plain);
	error = QObject::tr("A descriptografia deste backup não está disponível nesta plataforma.");
	return false;
#endif
}
