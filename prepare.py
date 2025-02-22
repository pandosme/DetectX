import json
import os
import tensorflow as tf

def get_chip_name(platform):
    platform_mapping = {
        "A8": "axis-a8-dlpu-tflite",
        "A9": "a9-dlpu-tflite",
        "TPU": "google-edge-tpu-tflite"
    }
    return platform_mapping.get(platform.upper(), "axis-a8-dlpu-tflite")

def get_video_dimensions(image_size):
    video_mapping = {
        480: 640,
        640: 800,  # New mapping for 640
        768: 1024,
        960: 1280
    }
    video_height_mapping = {
        640: 600  # Special case for height when image_size is 640
    }
    
    # Get video width from mapping or default to 640
    video_width = video_mapping.get(image_size, 640)
    
    # Get video height from special mapping or use image_size as default
    video_height = video_height_mapping.get(image_size, image_size)
    
    return video_width, video_height

def parse_labels_file(file_path):
    try:
        with open(file_path, 'r') as file:
            labels = [line.strip() for line in file if line.strip()]
        return labels
    except FileNotFoundError:
        print(f"Warning: {file_path} not found. Using default labels.")
        return ["label1", "label2"]

def generate_json(platform="A8", image_size=480):
    # Get video dimensions
    video_width, video_height = get_video_dimensions(image_size)
    
    # Default values
    data = {
        "modelWidth": image_size,
        "modelHeight": image_size,
        "quant": 0,
        "zeroPoint": 0,
        "boxes": 0,
        "classes": 0,
        "objectness": 0.25,
        "nms": 0.05,
        "path": "model/model.tflite",
        "scaleMode": 0,
        "videoWidth": video_width,
        "videoHeight": video_height,
        "videoAspect": "4:3",
        "chip": get_chip_name(platform),
        "labels": ["label1", "label2"],
        "description": ""
    }

    # Rest of the function remains the same
    labels_path = "./app/model/labels.txt"
    data["labels"] = parse_labels_file(labels_path)

    model_path = "./app/model/model.tflite"
    try:
        interpreter = tf.lite.Interpreter(model_path=model_path)
        interpreter.allocate_tensors()

        input_details = interpreter.get_input_details()
        input_shape = input_details[0]['shape']
        data["modelWidth"] = image_size
        data["modelHeight"] = image_size

        output_details = interpreter.get_output_details()
        
        scale, zero_point = output_details[0]['quantization']
        box_number = output_details[0]['shape'][1]
        class_number = output_details[0]['shape'][2] - 5

        data["quant"] = float(scale)
        data["zeroPoint"] = int(zero_point)
        data["boxes"] = int(box_number)
        data["classes"] = int(class_number)

    except Exception as e:
        print(f"Warning: Error processing TFLite model: {e}")

    if len(data["labels"]) != data["classes"]:
        print(f"Warning: Number of labels ({len(data['labels'])}) does not match number of classes ({data['classes']}).")

    os.makedirs('./app/model', exist_ok=True)

    file_path = './app/model/model.json'
    with open(file_path, 'w') as json_file:
        json.dump(data, json_file, indent=2)

    print(f"JSON file has been generated and saved to {file_path}")
    print("\nGenerated JSON:")
    print(json.dumps(data, indent=2))

# Rest of the code remains the same
def generate_settings_json():
    # ... (keep the existing generate_settings_json function unchanged)
    pass

if __name__ == "__main__":
    platform = input("Enter platform (A8/A9/TPU): ")
    image_size = int(input("Enter image size (480/640/768/960): "))
    
    generate_json(platform, image_size)
    
    labels_path = "./app/model/labels.txt"
    labels = parse_labels_file(labels_path)
    
    generate_settings_json()
    