# public_send of a private / protected method names the receiver the way a plain
# call does: "an instance of K", or its #inspect once it has a singleton class.
class PubSendK
  private def secret = 1
  protected def guarded = 2
end
def pub_send_msg = yield rescue $!.message

p pub_send_msg { PubSendK.new.public_send(:secret) }
p pub_send_msg { PubSendK.new.public_send(:guarded) }
p pub_send_msg { PubSendK.new.secret }
o = Object.new
def o.hidden = 3
o.singleton_class.send(:private, :hidden)
p pub_send_msg { o.public_send(:hidden) }.sub(/0x\h+/, "0x")
p pub_send_msg { o.hidden }.sub(/0x\h+/, "0x")
