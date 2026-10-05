"""A chat in ten lines: python examples/chat.py <catalog id or model.gguf>"""
import sys
import geistr

with geistr.chat(sys.argv[1], system="Answer in one short sentence.") as chat:
    for question in ["What is the capital of France?", "And of Italy?"]:
        print(">", question)
        for piece in chat.send(question):  # only the new message is processed
            print(piece, end="", flush=True)
        print()
