import json
import os
import tensorflow as tf

def parse_labels_file(file_path):
    try:
        with open(file_path, 'r') as file:
            labels = [line.strip() for line in file if line.strip()]
        return labels
    except FileNotFoundError:
        print(f"Warning: {file_path} not found. Using default labels.")
        return ["label1", "label2"]

def generate_json():
    # Default values
    data = {
        "modelWidth": 0,
        "modelHeight": 640,
        "quant": 0,
        "zeroPoint": 0,
        "boxes": 0,
        "classes": 0,
        "objectness": 0.25,
        "nms": 0.05,
        "path": "model/model.tflite",
        "scaleMode": 0,
        "videoWidth": 1280,
        "videoHeight": 720,
        "chip": "axis-a8-dlpu-tflite",
        "labels": ["label1", "label2"]
    }

    # 1. Parse labels file
    labels_path = "./app/model/labels.txt"
    data["labels"] = parse_labels_file(labels_path)

    # 2. Use TensorFlow to get model details
    model_path = "./app/model/model.tflite"
    try:
        interpreter = tf.lite.Interpreter(model_path=model_path)
        interpreter.allocate_tensors()

        # Get input details to extract size
        input_details = interpreter.get_input_details()
        input_shape = input_details[0]['shape']
        # Assuming the input is square, use the height (or width) as the size
        data["modelWidth"] = int(input_shape[1])
        data["modelHeight"] = int(input_shape[2]) 

        output_details = interpreter.get_output_details()
        
        # 3. Update properties from interpreter output
        scale, zero_point = output_details[0]['quantization']
        box_number = output_details[0]['shape'][1]
        class_number = output_details[0]['shape'][2] - 5  # removing 5 values that are x,y,w,h,obj_conf

        data["quant"] = float(scale)  # Convert to float for JSON serialization
        data["zeroPoint"] = int(zero_point)
        data["boxes"] = int(box_number)
        data["classes"] = int(class_number)

    except Exception as e:
        print(f"Warning: Error processing TFLite model: {e}")

    # 4. Verify labels count
    if len(data["labels"]) != data["classes"]:
        print(f"Warning: Number of labels ({len(data['labels'])}) does not match number of classes ({data['classes']}).")

    # Ensure the directory exists
    os.makedirs('./app/html/config', exist_ok=True)

    # Save the JSON file
    file_path = './app/html/config/model.json'
    with open(file_path, 'w') as json_file:
        json.dump(data, json_file, indent=2)

    print(f"JSON file has been generated and saved to {file_path}")

    # 5. Print JSON to console
    print("\nGenerated JSON:")
    print(json.dumps(data, indent=2))

def generate_settings_json():
    settings = {
        "confidence": 50,
        "aoi": {
            "x1": 100,
            "y1": 100,
            "x2": 900,
            "y2": 900
        },
        "ignore": [],
        "sdcard": {
          "capture": False,
          "width": 1280,
          "height": 720
        }
    }

    # Ensure the directory exists
    os.makedirs('./app/html/config', exist_ok=True)

    # Save the settings JSON file
    settings_file_path = './app/html/config/settings.json'
    with open(settings_file_path, 'w') as settings_json_file:
        json.dump(settings, settings_json_file, indent=2)

    print(f"Settings JSON file has been generated and saved to {settings_file_path}")

    # 6. Print settings JSON to console
    print("\nGenerated Settings JSON:")
    print(json.dumps(settings, indent=2))

if __name__ == "__main__":
    generate_json()
    
    # Parse labels for settings.json
    labels_path = "./app/model/labels.txt"
    labels = parse_labels_file(labels_path)
    
    generate_settings_json()
