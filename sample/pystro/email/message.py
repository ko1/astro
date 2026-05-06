"""pystro stub for `email.message`."""


class Message:
    def __init__(self, *args, **kwargs):
        self._headers = []
        self._payload = None
    def __getitem__(self, name):
        for k, v in self._headers:
            if k.lower() == name.lower(): return v
        return None
    def __setitem__(self, name, value):
        self._headers.append((name, value))
    def __delitem__(self, name):
        self._headers = [(k, v) for k, v in self._headers if k.lower() != name.lower()]
    def __contains__(self, name):
        return any(k.lower() == name.lower() for k, v in self._headers)
    def keys(self): return [k for k, _ in self._headers]
    def values(self): return [v for _, v in self._headers]
    def items(self): return list(self._headers)
    def get(self, name, failobj=None):
        for k, v in self._headers:
            if k.lower() == name.lower(): return v
        return failobj
    def get_all(self, name, failobj=None):
        out = [v for k, v in self._headers if k.lower() == name.lower()]
        return out or failobj
    def add_header(self, _name, _value, **params):
        self._headers.append((_name, _value))
    def set_payload(self, payload, charset=None):
        self._payload = payload
    def get_payload(self, i=None, decode=False):
        return self._payload
    def is_multipart(self): return False
    def get_content_type(self): return "text/plain"
    def get_content_maintype(self): return "text"
    def get_content_subtype(self): return "plain"
    def as_string(self, *a, **kw): return ""
    def as_bytes(self, *a, **kw): return b""


class EmailMessage(Message): pass
class MIMEPart(Message): pass


__all__ = ["Message", "EmailMessage", "MIMEPart"]
