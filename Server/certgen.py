# -*- coding: utf-8 -*-
"""自签名 TLS 证书生成与加载（依赖 cryptography 库）。

用于服务器 HTTPS：无需外部 openssl。生成 RSA 2048 自签名证书，缓存到
data_dir/ssl/server.crt + server.key；也可加载用户提供的证书文件。
"""
import datetime
import os


def generate_self_signed(common_name="CloudUpdate", country="CN", days=3650, key_size=2048):
    from cryptography import x509
    from cryptography.x509.oid import NameOID
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import rsa

    key = rsa.generate_private_key(public_exponent=65537, key_size=key_size)
    subject = issuer = x509.Name([
        x509.NameAttribute(NameOID.COUNTRY_NAME, country),
        x509.NameAttribute(NameOID.COMMON_NAME, common_name),
    ])
    now = datetime.datetime.now(datetime.timezone.utc)
    builder = (x509.CertificateBuilder()
               .subject_name(subject)
               .issuer_name(issuer)
               .public_key(key.public_key())
               .serial_number(x509.random_serial_number())
               .add_extension(x509.SubjectAlternativeName([x509.DNSName("localhost")]), critical=False))
    try:
        cert = builder.not_valid_before(now).not_valid_after(
            now + datetime.timedelta(days=days)).sign(key, hashes.SHA256())
    except AttributeError:
        # 旧版 cryptography 接受 naive datetime
        naive = now.replace(tzinfo=None)
        cert = builder.not_valid_before(naive).not_valid_after(
            naive + datetime.timedelta(days=days)).sign(key, hashes.SHA256())
    cert_pem = cert.public_bytes(serialization.Encoding.PEM)
    key_pem = key.private_bytes(serialization.Encoding.PEM,
                                serialization.PrivateFormat.TraditionalOpenSSL,
                                serialization.NoEncryption())
    return cert_pem, key_pem


def ensure_self_signed_cert(ssl_dir, common_name="CloudUpdate", country="CN", days=3650):
    """生成并缓存自签名证书；已存在且未过期则直接返回路径。返回 (cert_path, key_path)。"""
    os.makedirs(ssl_dir, exist_ok=True)
    cert_path = os.path.join(ssl_dir, "server.crt")
    key_path = os.path.join(ssl_dir, "server.key")
    if os.path.isfile(cert_path) and os.path.isfile(key_path) and not _cert_expired(cert_path):
        return cert_path, key_path
    cert_pem, key_pem = generate_self_signed(common_name=common_name, country=country, days=days)
    with open(cert_path, "wb") as f:
        f.write(cert_pem)
    with open(key_path, "wb") as f:
        f.write(key_pem)
    # 私钥仅限属主读写，避免其他本地用户读取
    try:
        os.chmod(key_path, 0o600)
    except OSError:
        pass
    return cert_path, key_path


def _cert_expired(cert_path, leeway_days=7):
    """证书是否将在 leeway_days 内过期（或已损坏）。过期则触发重新生成。"""
    try:
        from cryptography import x509
        with open(cert_path, "rb") as f:
            cert = x509.load_pem_x509_certificate(f.read())
        try:
            not_after = cert.not_valid_after_utc
        except AttributeError:
            not_after = cert.not_valid_after
        now = datetime.datetime.now(datetime.timezone.utc)
        if not_after.tzinfo is None:
            not_after = not_after.replace(tzinfo=datetime.timezone.utc)
        return not_after <= now + datetime.timedelta(days=leeway_days)
    except Exception:
        # 解析失败（文件损坏/非证书）视为需要重新生成
        return True


def load_cert_pair(cert_file, key_file):
    """加载用户提供的证书。返回 (cert_path, key_path) 或抛错。"""
    if not (cert_file and key_file and os.path.isfile(cert_file) and os.path.isfile(key_file)):
        raise RuntimeError(f"证书文件不存在或路径为空：cert={cert_file} key={key_file}")
    return cert_file, key_file
